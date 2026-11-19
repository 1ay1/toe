// SPDX-License-Identifier: LGPL-2.0-or-later
//
// Template implementation of toe::vt::Parser. Included by parser.hpp.

#ifndef TOE_VT_PARSER_INL
#define TOE_VT_PARSER_INL

namespace toe::vt {

namespace detail {
// The two actions that make up nearly every byte of real output -- a run of
// printable ASCII and a C0 control (CR, LF, BS, HT) -- go straight to a sink
// that can take them (`print_run(sv)`, `control(byte)`), without building a
// 64-byte Action variant and dispatching it through a visit table. A plain
// callable sink still gets every Action, as before.
template <typename Sink>
inline void emit_run(Sink &sink, std::string_view text) {
    if constexpr (requires { sink.print_run(text); }) sink.print_run(text);
    else sink(Action{PrintRun{text}});
}
template <typename Sink>
inline void emit_control(Sink &sink, std::uint8_t b) {
    if constexpr (requires { sink.control(b); }) sink.control(b);
    else sink(Action{Execute{b}});
}
} // namespace detail

inline void Parser::clear_csi() noexcept {
    params_.clear();
    param_sub_.clear();
    param_started_ = false;
    intermediates_.clear();
    marker_ = 0;
    private_marker_ = false;
}

template <typename Sink>
void Parser::csi_dispatch(std::uint8_t final, Sink &sink) {
    // A trailing sub-parameter with no digits still counts as a default 0.
    if (param_started_ && params_.empty()) {
        params_.push_back(0);
        param_sub_.push_back(0);
    }
    CsiDispatch d;
    d.params = std::span<const int>{params_};
    d.sub = std::span<const std::uint8_t>{param_sub_};
    d.intermediates = intermediates_;
    d.final = static_cast<char>(final);
    d.private_marker = private_marker_;
    d.marker = marker_;
    sink(Action{d});
}

// UTF-8 aware handling of a Ground-state byte.
//
// Validation follows the Unicode standard's definition of well-formed UTF-8
// (Table 3-7): a decoder must reject overlong encodings, UTF-16 surrogates
// (U+D800..U+DFFF) and anything above U+10FFFF, substituting U+FFFD. Accepting
// them is a security problem, not just a cosmetic one — an overlong "/" or a
// smuggled NUL can slip past a filter that checks the decoded text.
template <typename Sink>
void Parser::ground_byte(std::uint8_t b, Sink &sink) {
    if (utf8_remaining_ > 0) {
        if ((b & 0xC0) == 0x80) { // continuation byte
            // Reject as soon as the prefix can't lead anywhere valid, so a bad
            // sequence costs one U+FFFD rather than silently decoding. The
            // SECOND byte of the sequence is the one that arrives while the
            // full count is still outstanding.
            if (utf8_remaining_ == utf8_need_ && !utf8_second_ok(b)) {
                sink(Action{Print{U'\uFFFD'}});
                utf8_remaining_ = 0;
                return; // the byte was consumed as part of the bad sequence
            }
            utf8_cp_ = (utf8_cp_ << 6) | (b & 0x3F);
            if (--utf8_remaining_ == 0) {
                sink(Action{Print{utf8_cp_}});
            }
            return;
        }
        // Malformed: emit replacement, fall through to reinterpret `b`.
        sink(Action{Print{U'\uFFFD'}});
        utf8_remaining_ = 0;
    }

    if (b < 0x80) {
        sink(Action{Print{static_cast<char32_t>(b)}});
    } else if ((b & 0xE0) == 0xC0) {
        // C0/C1 are overlong leads (they could only encode < U+0080).
        if (b < 0xC2) {
            sink(Action{Print{U'\uFFFD'}});
            return;
        }
        utf8_cp_ = b & 0x1F;
        utf8_remaining_ = utf8_need_ = 1;
    } else if ((b & 0xF0) == 0xE0) {
        utf8_cp_ = b & 0x0F;
        utf8_remaining_ = utf8_need_ = 2;
    } else if ((b & 0xF8) == 0xF0) {
        // F5..F7 would start a codepoint above U+10FFFF.
        if (b > 0xF4) {
            sink(Action{Print{U'\uFFFD'}});
            return;
        }
        utf8_cp_ = b & 0x07;
        utf8_remaining_ = utf8_need_ = 3;
    } else {
        // 0x80..0xBF stray continuation, or 0xF8..0xFF (never valid UTF-8).
        sink(Action{Print{U'\uFFFD'}});
    }
}

template <typename Sink>
void Parser::step(std::uint8_t b, Sink &sink) {
    // ESC (0x1B) aborts most states and restarts a sequence. OSC and DCS
    // strings are terminated BY ESC (as part of ST, ESC \), so they handle it
    // themselves rather than aborting.
    if (b == 0x1B && state_ != State::OscString && state_ != State::DcsPassthrough &&
        state_ != State::DcsIgnore && state_ != State::ApcString) {
        state_ = State::Escape;
        return;
    }

    switch (state_) {
    case State::Ground:
        if (b < 0x20 || b == 0x7F) {
            detail::emit_control(sink, b);
        } else {
            ground_byte(b, sink);
        }
        break;

    case State::Escape:
        clear_csi();
        osc_.clear();
        if (b == '[') {
            state_ = State::CsiEntry;
        } else if (b == ']') {
            state_ = State::OscString;
        } else if (b == 'P') { // DCS (Device Control String)
            dcs_prefix_.clear();
            dcs_data_.clear();
            dcs_saw_esc_ = false;
            params_.clear();
            state_ = State::DcsEntry;
        } else if (b == '_') { // APC (kitty graphics)
            apc_.clear();
            apc_saw_esc_ = false;
            state_ = State::ApcString;
        } else if (b >= 0x20 && b <= 0x2F) { // intermediate
            intermediates_.push_back(static_cast<char>(b));
            state_ = State::EscapeIntermediate;
        } else if (b >= 0x30 && b <= 0x7E) { // final
            sink(Action{EscDispatch{intermediates_, static_cast<char>(b)}});
            state_ = State::Ground;
        } else if (b < 0x20) {
            sink(Action{Execute{b}});
        }
        break;

    case State::EscapeIntermediate:
        if (b >= 0x20 && b <= 0x2F) {
            intermediates_.push_back(static_cast<char>(b));
        } else if (b >= 0x30 && b <= 0x7E) {
            sink(Action{EscDispatch{intermediates_, static_cast<char>(b)}});
            state_ = State::Ground;
        } else if (b < 0x20) {
            sink(Action{Execute{b}});
        }
        break;

    case State::CsiEntry:
        if (b < 0x20) {
            sink(Action{Execute{b}});
        } else if (b >= 0x3C && b <= 0x3F) { // private marker
            private_marker_ = true;
            marker_ = static_cast<char>(b);
            state_ = State::CsiParam;
        } else if ((b >= 0x30 && b <= 0x39) || b == ';') {
            state_ = State::CsiParam;
            step(b, sink); // reprocess as a param byte
        } else if (b >= 0x20 && b <= 0x2F) {
            intermediates_.push_back(static_cast<char>(b));
            state_ = State::CsiIntermediate;
        } else if (b >= 0x40 && b <= 0x7E) {
            csi_dispatch(b, sink);
            state_ = State::Ground;
        } else {
            state_ = State::CsiIgnore;
        }
        break;

    case State::CsiParam:
        if (b < 0x20) {
            sink(Action{Execute{b}});
        } else if (b >= 0x30 && b <= 0x39) { // digit
            if (!param_started_) {
                params_.push_back(0);
                param_sub_.push_back(0);
                param_started_ = true;
            }
            // Clamp instead of overflowing: a child can send arbitrarily many
            // digits, and signed overflow is UB. xterm caps parameters too, and
            // every consumer already clamps to the screen, so a saturated value
            // behaves the same as a huge one.
            if (params_.back() < kParamMax)
                params_.back() = params_.back() * 10 + (b - '0');
        } else if (b == ';' || b == ':') {
            // Close the current param (default 0 if empty) and open a fresh
            // empty slot. ';' starts a new primary parameter; ':' opens a
            // SUB-parameter of the current one (SGR 4:3, 38:2:r:g:b).
            const std::uint8_t sub = (b == ':') ? 1 : 0;
            if (!param_started_) {
                params_.push_back(0);
                param_sub_.push_back(0);
            }
            params_.push_back(0);
            param_sub_.push_back(sub);
            param_started_ = true; // the freshly-opened slot counts as started
        } else if (b >= 0x20 && b <= 0x2F) {
            intermediates_.push_back(static_cast<char>(b));
            state_ = State::CsiIntermediate;
        } else if (b >= 0x40 && b <= 0x7E) {
            csi_dispatch(b, sink);
            state_ = State::Ground;
        } else {
            state_ = State::CsiIgnore;
        }
        break;

    case State::CsiIntermediate:
        if (b < 0x20) {
            sink(Action{Execute{b}});
        } else if (b >= 0x20 && b <= 0x2F) {
            intermediates_.push_back(static_cast<char>(b));
        } else if (b >= 0x40 && b <= 0x7E) {
            csi_dispatch(b, sink);
            state_ = State::Ground;
        } else {
            state_ = State::CsiIgnore;
        }
        break;

    case State::CsiIgnore:
        if (b >= 0x40 && b <= 0x7E) {
            state_ = State::Ground;
        }
        break;

    case State::OscString:
        // Terminated by BEL (0x07) or ST (ESC \ — 0x1B 0x5C, handled below).
        if (b == 0x07) {
            sink(Action{OscDispatch{osc_}});
            state_ = State::Ground;
        } else if (b == 0x1B) {
            // Peek for ST: stash and wait. Simplest correct approach: treat a
            // lone ESC as end-of-OSC start; the next '\' completes it. We set
            // a tiny sub-state via marker_.
            marker_ = 1; // "saw ESC in OSC"
        } else if (marker_ == 1) {
            if (b == '\\') {
                sink(Action{OscDispatch{osc_}});
            }
            marker_ = 0;
            state_ = State::Ground;
        } else if (b >= 0x20) {
            osc_.push_back(static_cast<char>(b));
        }
        break;

    case State::DcsEntry:
        // Collect params, intermediates and the final byte that selects the DCS
        // kind (e.g. "+q" for XTGETTCAP, "$q" for DECRQSS). The prefix is the
        // intermediate(s) + final; everything after is passthrough data.
        if ((b >= 0x30 && b <= 0x39) || b == ';' || b == ':' ||
            (b >= 0x3C && b <= 0x3F)) {
            dcs_prefix_.push_back(static_cast<char>(b)); // params folded into prefix
        } else if (b >= 0x20 && b <= 0x2F) {             // intermediate
            dcs_prefix_.push_back(static_cast<char>(b));
        } else if (b >= 0x40 && b <= 0x7E) {             // final -> passthrough
            dcs_prefix_.push_back(static_cast<char>(b));
            state_ = State::DcsPassthrough;
        } else if (b == 0x1B) {
            dcs_saw_esc_ = true;
            state_ = State::DcsPassthrough;
        } else {
            state_ = State::DcsIgnore;
        }
        break;

    case State::DcsPassthrough:
        if (dcs_saw_esc_) {
            // Completing ST: ESC \ finishes the DCS.
            if (b == '\\') {
                sink(Action{DcsDispatch{dcs_prefix_, dcs_data_}});
            }
            dcs_saw_esc_ = false;
            state_ = State::Ground;
        } else if (b == 0x1B) {
            dcs_saw_esc_ = true;
        } else if (b == 0x9C) { // 8-bit ST
            sink(Action{DcsDispatch{dcs_prefix_, dcs_data_}});
            state_ = State::Ground;
        } else {
            dcs_data_.push_back(static_cast<char>(b));
        }
        break;

    case State::DcsIgnore:
        if (b == 0x1B) {
            dcs_saw_esc_ = true;
        } else if (dcs_saw_esc_ && b == '\\') {
            dcs_saw_esc_ = false;
            state_ = State::Ground;
        } else {
            dcs_saw_esc_ = false;
        }
        break;

    case State::ApcString:
        // ESC _ <data> ST. Collect the payload until ST (ESC \ or 8-bit 0x9C).
        // kitty graphics packets can be large, so this just accumulates bytes.
        if (apc_saw_esc_) {
            if (b == '\\') {
                sink(Action{ApcDispatch{apc_}});
            }
            apc_saw_esc_ = false;
            state_ = State::Ground;
        } else if (b == 0x1B) {
            apc_saw_esc_ = true;
        } else if (b == 0x9C) { // 8-bit ST
            sink(Action{ApcDispatch{apc_}});
            state_ = State::Ground;
        } else {
            apc_.push_back(static_cast<char>(b));
        }
        break;
    }
}

template <typename Sink>
const std::uint8_t *Parser::csi_fast(const std::uint8_t *p, const std::uint8_t *end, Sink &sink) {
    // p points at ESC. The shape: ESC '[' [<=>?]? ( [0-9:;] )* [@-~]
    if (end - p < 3 || p[1] != '[') return nullptr;
    const std::uint8_t *q = p + 2;
    char marker = 0;
    if (*q >= 0x3C && *q <= 0x3F) marker = static_cast<char>(*q++);
    else if (*q == ':') return nullptr;   // ':' straight after CSI: the sequence is ignored (state machine)
    params_.clear();
    param_sub_.clear();
    bool started = false;
    int cur = 0;
    std::uint8_t sub = 0;
    for (; q < end; ++q) {
        const std::uint8_t b = *q;
        if (b >= '0' && b <= '9') {
            if (cur < kParamMax) cur = cur * 10 + (b - '0');
            started = true;
        } else if (b == ';' || b == ':') {
            params_.push_back(cur);
            param_sub_.push_back(sub);
            sub = b == ':' ? 1 : 0;
            cur = 0;
            started = true;
        } else if (b >= 0x40 && b <= 0x7E) {
            if (started) {
                params_.push_back(cur);
                param_sub_.push_back(sub);
            }
            intermediates_.clear();
            CsiDispatch d;
            d.params = std::span<const int>{params_};
            d.sub = std::span<const std::uint8_t>{param_sub_};
            d.intermediates = intermediates_;
            d.final = static_cast<char>(b);
            d.private_marker = marker != 0;
            d.marker = marker;
            sink(Action{d});
            state_ = State::Ground;
            return q + 1;
        } else {
            return nullptr;   // intermediates, controls, a nested ESC: the full machine
        }
    }
    return nullptr;   // split across buffers: the state machine carries it over
}

template <typename Sink>
void Parser::feed(std::span<const char> bytes, Sink &&sink) {
    const auto *p = reinterpret_cast<const std::uint8_t *>(bytes.data());
    const auto *const end = p + bytes.size();
    while (p < end) {
        // Ground-state fast path: the overwhelmingly common case is a run of
        // ordinary printable bytes (plain text, and the ASCII inside escape-
        // heavy output). When we're in Ground with no partial UTF-8 sequence,
        // scan the whole printable-ASCII run at once and emit it directly,
        // skipping the per-byte ESC guard + state switch of step(). Anything
        // else (control, ESC, or a UTF-8 lead byte >= 0x80) drops back to the
        // full state machine for a single byte, then we resume scanning.
        if (state_ == State::Ground && utf8_remaining_ == 0) {
            const std::uint8_t *run = p;
            // SIMD: skip to the first non-printable-ASCII byte in one sweep
            // (16 bytes/iter on NEON/SSE2) instead of a byte-at-a-time loop.
            p += printable_ascii_run(p, static_cast<std::size_t>(end - p));
            if (run != p) {
                // Emit the whole run as one action — the screen writes it in a
                // tight loop without per-char variant/visit/apply overhead.
                detail::emit_run(sink, std::string_view{reinterpret_cast<const char *>(run),
                                                        static_cast<std::size_t>(p - run)});
            }
            if (p == end) break;
            // A C0 control in Ground (CR, LF, BS, HT -- not ESC, which starts a
            // sequence): no state change, straight to the sink. `yes` and
            // every line of output is run, CR, LF, run, CR, LF...
            if (*p < 0x20 && *p != 0x1B) {
                detail::emit_control(sink, *p++);
                continue;
            }
            // ESC [ ... final, whole in this buffer (SGR colours, cursor
            // moves): parse it in one pass here instead of a state-machine
            // step per byte. Anything unusual -- intermediates, a split
            // across buffers, a control inside -- falls through to step().
            if (*p == 0x1B) {
                if (const std::uint8_t *q = csi_fast(p, end, sink)) { p = q; continue; }
            }
        }
        step(*p++, sink);
    }
}

} // namespace toe::vt

#endif // TOE_VT_PARSER_INL
