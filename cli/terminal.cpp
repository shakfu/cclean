#include "terminal.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>

#include <unistd.h>

namespace cclean::cli {

bool is_terminal(int descriptor) {
    return isatty(descriptor) == 1;
}

Style Style::detect(int descriptor, ColorWhen when) {
    if (when == ColorWhen::Never) {
        return Style{};
    }

    if (when == ColorWhen::Auto) {
        const char* const no_color = std::getenv("NO_COLOR");

        if (!is_terminal(descriptor) || (no_color && no_color[0] != '\0')) {
            return Style{};
        }
    }

    Style style;
    style.reset = "\033[0m";
    style.dim = "\033[2m";
    style.bold = "\033[1m";
    style.directory = "\033[36m";
    style.warning = "\033[33m";
    style.failure = "\033[31m";
    style.success = "\033[32m";
    return style;
}

void Progress::update(const char* phase, std::uintmax_t done) {
    if (!enabled_ || ++calls_ % 256 != 0) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();

    if (now - drawn_ < std::chrono::milliseconds(80)) {
        return;
    }

    drawn_ = now;
    draw(phase, done);
}

void Progress::finish() {
    if (!enabled_ || width_ == 0) {
        return;
    }

    std::fprintf(stderr, "\r%*s\r", static_cast<int>(width_), "");
    std::fflush(stderr);
    width_ = 0;
}

void Progress::draw(const char* phase, std::uintmax_t done) {
    char line[128];
    const int written = std::snprintf(line, sizeof(line), "  %s %llu",
                                      phase,
                                      static_cast<unsigned long long>(done));

    if (written <= 0) {
        return;
    }

    const std::size_t length = static_cast<std::size_t>(written);
    const std::size_t padding = length < width_ ? width_ - length : 0;

    std::fprintf(stderr, "\r%s%*s", line, static_cast<int>(padding), "");
    std::fflush(stderr);
    width_ = length;
}

TerminalMode::TerminalMode(int descriptor) : descriptor_(descriptor) {
    saved_ = tcgetattr(descriptor_, &original_) == 0;
}

TerminalMode::~TerminalMode() {
    restore();
}

bool TerminalMode::raw() {
    if (!saved_) {
        return false;
    }

    termios mode = original_;
    mode.c_lflag &= static_cast<tcflag_t>(~(ICANON | ECHO));
    mode.c_cc[VMIN] = 1;
    mode.c_cc[VTIME] = 0;

    // TCSAFLUSH discards anything typed ahead, so a stray keystroke from
    // before the prompt cannot stand in for the answer.
    if (tcsetattr(descriptor_, TCSAFLUSH, &mode) != 0) {
        return false;
    }

    changed_ = true;
    return true;
}

void TerminalMode::restore() {
    if (!changed_) {
        return;
    }

    changed_ = false;

    while (tcsetattr(descriptor_, TCSAFLUSH, &original_) != 0) {
        // A signal arriving mid-call is the one failure worth retrying.
        if (errno == EINTR) {
            continue;
        }

        std::cerr << "Warning: could not restore the terminal: "
                  << std::strerror(errno)
                  << "\n  Run 'stty sane' if typing stops echoing.\n";
        return;
    }
}

bool confirmed() {
    if (!is_terminal(STDIN_FILENO)) {
        const int key = std::getchar();
        return key == 'y' || key == 'Y';
    }

    TerminalMode terminal(STDIN_FILENO);

    if (!terminal.raw()) {
        // Without raw mode the answer needs Enter, which is still an answer.
        const int key = std::getchar();
        return key == 'y' || key == 'Y';
    }

    const int key = std::getchar();

    terminal.restore();

    return key == 'y' || key == 'Y';
}

}  // namespace cclean::cli
