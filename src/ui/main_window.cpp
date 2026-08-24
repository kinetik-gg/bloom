#include <bloom/ui/main_window.hpp>

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QComboBox>
#include <QDockWidget>
#include <QFrame>
#include <QLabel>
#include <QMenuBar>
#include <QPalette>
#include <QSizePolicy>
#include <QSplitter>
#include <QStatusBar>
#include <QVBoxLayout>
#include <QWidget>

namespace {

QWidget* makeEditorPanel(const QString& editorName, const QString& placeholder, QWidget* parent) {
    auto* panel = new QFrame(parent);
    panel->setObjectName("editorPanel");

    auto* layout = new QVBoxLayout(panel);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto* content = new QLabel(placeholder, panel);
    content->setAlignment(Qt::AlignCenter);
    content->setObjectName("editorPlaceholder");
    content->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    auto* header = new QWidget(panel);
    header->setObjectName("editorHeader");
    auto* headerLayout = new QVBoxLayout(header);
    headerLayout->setContentsMargins(8, 5, 8, 5);

    auto* editorPicker = new QComboBox(header);
    editorPicker->addItems({"Compositor", "Nodes", "Timeline", "Media", "Properties"});
    editorPicker->setCurrentText(editorName);
    editorPicker->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    headerLayout->addWidget(editorPicker, 0, Qt::AlignLeft);

    QObject::connect(editorPicker, &QComboBox::currentTextChanged, content,
                     [content](const QString& selectedEditor) {
                         content->setText(selectedEditor + "\n\nEditor scaffold");
                     });

    layout->addWidget(header);
    layout->addWidget(content, 1);
    return panel;
}

QDockWidget* makeDock(const QString& title, const QString& placeholder, QWidget* parent,
                      Qt::DockWidgetAreas areas) {
    auto* dock = new QDockWidget(title, parent);
    dock->setObjectName(title.toLower() + "Dock");
    dock->setAllowedAreas(areas);
    dock->setFeatures(QDockWidget::DockWidgetClosable | QDockWidget::DockWidgetMovable |
                      QDockWidget::DockWidgetFloatable);
    dock->setWidget(makeEditorPanel(title, placeholder, dock));
    return dock;
}

} // namespace

namespace bloom::ui {

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setObjectName("bloomMainWindow");
    setWindowTitle("Bloom");
    resize(1600, 1000);
    setDockOptions(QMainWindow::AllowNestedDocks | QMainWindow::AllowTabbedDocks |
                   QMainWindow::AnimatedDocks);

    createMenus();
    createWorkspaceSwitcher();
    createEditorLayout();
    applyFoundationTheme();

    statusBar()->showMessage("Bloom C++/Qt foundation — no project loaded");
}

void MainWindow::createMenus() {
    menuBar()->addMenu("&File");
    menuBar()->addMenu("&Edit");
    menuBar()->addMenu("&View");
    menuBar()->addMenu("&Window");
    menuBar()->addMenu("&Help");
}

void MainWindow::createWorkspaceSwitcher() {
    menuBar()->addSeparator();

    auto* group = new QActionGroup(menuBar());
    group->setExclusive(true);

    const QStringList workspaces = {"Compositing", "Editing", "Grading", "Scripting", "Rendering"};
    for (const QString& workspace : workspaces) {
        auto* action = menuBar()->addAction(workspace);
        action->setCheckable(true);
        action->setEnabled(workspace == "Compositing");
        action->setChecked(workspace == "Compositing");
        group->addAction(action);
    }
}

void MainWindow::createEditorLayout() {
    auto* centralEditors = new QSplitter(Qt::Horizontal, this);
    centralEditors->setObjectName("centralEditors");
    centralEditors->addWidget(makeEditorPanel(
        "Compositor", "Viewer\n\nProject frame output appears here", centralEditors));
    centralEditors->addWidget(makeEditorPanel(
        "Nodes", "Node graph\n\nCanonical composition graph appears here", centralEditors));
    centralEditors->setStretchFactor(0, 1);
    centralEditors->setStretchFactor(1, 1);
    setCentralWidget(centralEditors);

    auto* sourceDock = makeDock("Media", "Assets and compositions", this,
                                Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    auto* propertiesDock = makeDock("Properties", "Selection-driven parameters", this,
                                    Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    auto* timelineDock = makeDock("Timeline", "Layers, animation, and current time", this,
                                  Qt::TopDockWidgetArea | Qt::BottomDockWidgetArea);

    addDockWidget(Qt::RightDockWidgetArea, sourceDock);
    addDockWidget(Qt::RightDockWidgetArea, propertiesDock);
    splitDockWidget(sourceDock, propertiesDock, Qt::Vertical);
    addDockWidget(Qt::BottomDockWidgetArea, timelineDock);

    resizeDocks({sourceDock, propertiesDock}, {260, 620}, Qt::Vertical);
    resizeDocks({timelineDock}, {340}, Qt::Vertical);
}

void MainWindow::applyFoundationTheme() {
    QPalette palette = QApplication::palette();
    palette.setColor(QPalette::Window, QColor(18, 18, 18));
    palette.setColor(QPalette::WindowText, QColor(215, 215, 215));
    palette.setColor(QPalette::Base, QColor(14, 14, 14));
    palette.setColor(QPalette::AlternateBase, QColor(25, 25, 25));
    palette.setColor(QPalette::Text, QColor(215, 215, 215));
    palette.setColor(QPalette::Button, QColor(28, 28, 28));
    palette.setColor(QPalette::ButtonText, QColor(215, 215, 215));
    palette.setColor(QPalette::Highlight, QColor(23, 142, 230));
    palette.setColor(QPalette::HighlightedText, Qt::white);
    QApplication::setPalette(palette);

    setStyleSheet(R"(
        QMainWindow, QMenuBar, QMenu, QStatusBar {
            background: #121212;
            color: #d7d7d7;
        }
        QMenuBar::item:checked {
            background: #303030;
            border-radius: 4px;
        }
        QDockWidget::title, #editorHeader {
            background: #171717;
            border-bottom: 1px solid #292929;
            padding: 4px;
        }
        #editorPanel {
            background: #111111;
            border: 1px solid #292929;
        }
        #editorPlaceholder {
            color: #777777;
        }
        QComboBox {
            background: #1b1b1b;
            border: 1px solid #303030;
            border-radius: 4px;
            padding: 3px 8px;
        }
        QSplitter::handle {
            background: #292929;
        }
    )");
}

} // namespace bloom::ui
