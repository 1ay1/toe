// SPDX-License-Identifier: LGPL-2.0-or-later
//
// Template implementation of gvte::vt::Parser. Included by parser.hpp.

#ifndef GVTE_VT_PARSER_INL
#define GVTE_VT_PARSER_INL

namespace gvte::vt {

inline void Parser::clear_csi() noexcept {
    params_.clear();
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
    }
    CsiDispatch d;
    d.params = std::span<const int>{params_};
    d.intermediates = intermediates_;
    d.final = static_cast<char>(final);
    d.private_marker = private_marker_;
    d.marker = marker_;
    sink(Action{d});
}

// UTF-8 aware handling of a Ground-state byte.
template <typename Sink>
void Parser::ground_byte(std::uint8_t b, Sink &sink) {
    if (utf8_remaining_ > 0) {
        if ((b & 0xC0) == 0x80) { // continuation byte
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
        utf8_cp_ = b & 0x1F;
        utf8_remaining_ = 1;
    } else if ((b & 0xF0) == 0xE0) {
        utf8_cp_ = b & 0x0F;
        utf8_remaining_ = 2;
    } else if ((b & 0xF8) == 0xF0) {
        utf8_cp_ = b & 0x07;
        utf8_remaining_ = 3;
    } else {
        sink(Action{Print{U'\uFFFD'}});
    }
}

template <typename Sink>
void Parser::step(std::uint8_t b, Sink &sink) {
    // ESC (0x1B) aborts most states and restarts a sequence. OSC and DCS
    // strings are terminated BY ESC (as part of ST, ESC \), so they handle it
    // themselves rather than aborting.
    if (b == 0x1B && state_ != State::OscString && state_ != State::DcsPassthrough &&
        state_ != State::DcsIgnore) {
        state_ = State::Escape;
        return;
    }

    switch (state_) {
    case State::Ground:
        if (b < 0x20 || b == 0x7F) {
            sink(Action{Execute{b}});
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
                param_started_ = true;
            }
            params_.back() = params_.back() * 10 + (b - '0');
        } else if (b == ';') {
            // Close the current param (default 0 if empty) and open a fresh
            // empty slot. `;H` -> {0,0}; `1;31m` -> {1,31}.
            if (!param_started_) {
                params_.push_back(0);
            }
            params_.push_back(0);
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
    }
}

template <typename Sink>
void Parser::feed(std::span<const char> bytes, Sink &&sink) {
    for (char c : bytes) {
        step(static_cast<std::uint8_t>(c), sink);
    }
}

} // namespace gvte::vt

#endif // GVTE_VT_PARSER_INL
