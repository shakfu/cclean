#ifndef CCLEAN_CLI_TERMINAL_HPP
#define CCLEAN_CLI_TERMINAL_HPP

#include <chrono>
#include <cstddef>
#include <cstdint>

#include <termios.h>

namespace cclean::cli {

bool is_terminal(int descriptor);

// What --color asked for. Auto is the default: colour when the stream is a
// terminal and NO_COLOR is unset.
enum class ColorWhen { Auto, Always, Never };

// Escape sequences resolve to empty strings off a terminal, so the same output
// code serves a pipe. NO_COLOR is honoured (https://no-color.org) unless
// --color=always overrides it, since a flag typed for one run is the more
// specific instruction.
struct Style {
    const char* reset = "";
    const char* dim = "";
    const char* bold = "";
    const char* directory = "";
    const char* warning = "";
    const char* failure = "";
    const char* success = "";

    static Style detect(int descriptor, ColorWhen when = ColorWhen::Auto);
};

// A single rewritten line on stderr, so a redirected target list stays clean.
// Without it a scan over a large tree looks like a hang.
class Progress {
public:
    explicit Progress(bool enabled)
        : enabled_(enabled),
          drawn_(std::chrono::steady_clock::now()) {}

    // Cheap enough to call per directory: the clock is only read once every
    // few hundred calls, and the line is redrawn at most every 80 ms.
    void update(const char* phase, std::uintmax_t done);

    void finish();

private:
    bool enabled_;
    std::chrono::steady_clock::time_point drawn_;
    std::uintmax_t calls_ = 0;
    std::size_t width_ = 0;

    void draw(const char* phase, std::uintmax_t done);
};

// Restores the terminal on every path out, the destructor included, so an
// exception or a return added later cannot leave it in raw no-echo mode. That
// failure is invisible: the shell comes back, and the user types the next
// command into a terminal that echoes nothing.
class TerminalMode {
public:
    explicit TerminalMode(int descriptor);
    ~TerminalMode();

    TerminalMode(const TerminalMode&) = delete;
    TerminalMode& operator=(const TerminalMode&) = delete;

    bool saved() const { return saved_; }

    bool raw();
    void restore();

private:
    int descriptor_;
    termios original_{};
    bool saved_ = false;
    bool changed_ = false;
};

// Takes one keypress without waiting for Enter. Falls back to reading a
// character from the stream when stdin is a pipe or a file, so scripts still
// work. Anything but y or Y cancels, end-of-input included.
bool confirmed();

}  // namespace cclean::cli

#endif  // CCLEAN_CLI_TERMINAL_HPP
