#pragma once
#include <QString>
#include <QStringList>
#include <filesystem>

class QObject;

namespace bloom::ui {
// Reveal in File Manager (#380): show a file selected in the platform's file manager where the
// platform allows it, otherwise open the folder that holds it.
enum class RevealPlatform { Linux, MacOS, Windows };

struct RevealPlan final {
    enum class Kind {
        // freedesktop.org FileManager1.ShowItems over the session bus, opening the folder if no
        // file manager answers.
        ShowItems,
        // Start `program` with `arguments` (or `nativeArguments` on Windows, which explorer parses
        // itself), opening the folder if it cannot start.
        Process,
    };
    Kind kind = Kind::ShowItems;
    QString program;
    QStringList arguments;
    QString nativeArguments;
    // file:// URIs for ShowItems.
    QStringList uris;
    // The folder holding the file: the fallback on every platform.
    QString folder;
};

[[nodiscard]] RevealPlatform currentRevealPlatform() noexcept;
// What revealing `file` does on `platform`. Pure, so every platform's choice is testable anywhere.
[[nodiscard]] RevealPlan revealPlan(RevealPlatform platform, const std::filesystem::path& file);
// Carries out the plan without blocking: the D-Bus call and the process start are asynchronous,
// and either failing opens the folder instead. `context` owns the pending call.
void revealInFileManager(const std::filesystem::path& file, QObject* context);
} // namespace bloom::ui
