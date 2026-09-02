#include <bloom/ui/licenses_window.hpp>
#include <bloom/ui/third_party_license_catalog.hpp>

#include <QApplication>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QString>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <set>
#include <source_location>
#include <string>
#include <string_view>

namespace {

class Expectations final {
  public:
    void expect(const bool condition, const std::string& message,
                const std::source_location location = std::source_location::current()) {
        if (condition) {
            return;
        }
        ++failures_;
        std::cerr << location.file_name() << ':' << location.line() << ": " << message << '\n';
    }

    [[nodiscard]] int failures() const noexcept { return failures_; }

  private:
    int failures_ = 0;
};

using namespace bloom::ui;

// The set of component directory names that carry a LICENSE file under one root -- exactly the
// discovery rule generate_third_party_license_catalog.cmake itself applies.
[[nodiscard]] std::set<std::string> licensedComponentDirectories(const std::filesystem::path& root) {
    std::set<std::string> names;
    if (!std::filesystem::exists(root)) {
        return names;
    }
    for (const auto& entry : std::filesystem::directory_iterator(root)) {
        if (entry.is_directory() && std::filesystem::exists(entry.path() / "LICENSE")) {
            names.insert(entry.path().filename().string());
        }
    }
    return names;
}

// Decision 3 (task U2, issue #118): the catalog lists AT LEAST every component directory present
// in the two license roots, and shows non-empty text for each. "At least" rather than "exactly"
// because the catalog also carries the synthesized Qt/ADR-0014 notice, which has no LICENSE file
// of its own to enumerate here.
void testCatalogCoversEveryLicensedComponentDirectory(Expectations& expectations,
                                                       const std::filesystem::path& repositoryRoot) {
    const auto dependencyComponents =
        licensedComponentDirectories(repositoryRoot / "dependencies" / "licenses");
    const auto kitComponents =
        licensedComponentDirectories(repositoryRoot / "src" / "ui" / "kit" / "third_party");
    expectations.expect(!dependencyComponents.empty(),
                        "the dependency license root really has components to require (fixture "
                        "sanity: a --root miss would silently pass an empty requirement)");
    expectations.expect(!kitComponents.empty(),
                        "the kit third-party license root really has components to require");

    std::set<std::string> catalogComponents;
    for (const auto& entry : thirdPartyLicenseCatalog()) {
        catalogComponents.emplace(entry.component);
        expectations.expect(!entry.licenseText.empty(),
                            std::string{"the catalog entry for "} + std::string{entry.component} +
                                " has non-empty license text");
    }

    for (const auto& name : dependencyComponents) {
        expectations.expect(catalogComponents.contains(name),
                            "the catalog lists dependency-license component: " + name);
    }
    for (const auto& name : kitComponents) {
        expectations.expect(catalogComponents.contains(name),
                            "the catalog lists kit third-party component: " + name);
    }
    expectations.expect(catalogComponents.contains("Qt 6"),
                        "the catalog also carries the Qt/ADR-0014 notice");
}

void testLicensesWindowListsEveryCatalogEntryWithText(Expectations& expectations) {
    LicensesWindow window;
    auto* list = window.findChild<QListWidget*>(QStringLiteral("licensesComponentList"));
    auto* text = window.findChild<QPlainTextEdit*>(QStringLiteral("licensesTextView"));
    expectations.expect(list != nullptr && text != nullptr,
                        "the window exposes its list and text panes");
    if (list == nullptr || text == nullptr) {
        return;
    }

    expectations.expect(static_cast<std::size_t>(list->count()) ==
                            thirdPartyLicenseCatalog().size(),
                        "the list has exactly one row per catalog entry");
    expectations.expect(!text->toPlainText().isEmpty(),
                        "selecting the first component (the default selection) shows its text");
    expectations.expect(text->isReadOnly(), "the license text is read-only");

    for (int row = 0; row < list->count(); ++row) {
        list->setCurrentRow(row);
        expectations.expect(!text->toPlainText().isEmpty(),
                            "every listed component shows non-empty license text when selected");
    }
}

} // namespace

int main(int argumentCount, char** arguments) {
    std::filesystem::path repositoryRoot = std::filesystem::current_path();
    for (int index = 1; index < argumentCount; ++index) {
        const std::string_view argument{arguments[index]};
        if (argument == "--root" && index + 1 < argumentCount) {
            repositoryRoot = arguments[++index];
        }
    }

    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argumentCount, arguments);
    Expectations expectations;
    testCatalogCoversEveryLicensedComponentDirectory(expectations, repositoryRoot);
    testLicensesWindowListsEveryCatalogEntryWithText(expectations);
    return expectations.failures() == 0 ? 0 : 1;
}
