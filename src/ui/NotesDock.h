//
// Dock "Notas IA (Markdown)": muestra y deja editar en vivo el .md que la IA va organizando.
//

#ifndef OBS_GOOGLE_CAPTION_PLUGIN_NOTESDOCK_H
#define OBS_GOOGLE_CAPTION_PLUGIN_NOTESDOCK_H

#include <QWidget>
#include <memory>
#include <string>

#include "../notes/MarkdownNotesEngine.h"

class QPlainTextEdit;
class QPushButton;
class QLabel;
class QLineEdit;
class QSpinBox;
class QGroupBox;
class QToolButton;
class CaptionPluginManager;
struct OutputCaptionResult;

class NotesDock : public QWidget {
Q_OBJECT
public:
    explicit NotesDock(CaptionPluginManager &plugin_manager, QWidget *parent = nullptr);

    ~NotesDock() override;

private slots:

    void on_start_stop_clicked();

    void on_open_folder_clicked();

    void on_browse_clicked();

    void on_editor_changed();

    void on_document_updated(const QString &markdown);

    void on_status(const QString &text);

    void on_engine_finished();

    void on_caption(std::shared_ptr<OutputCaptionResult> caption, bool cleared, std::string recent);

private:
    CaptionPluginManager &plugin_manager;
    MarkdownNotesEngine engine;

    QLabel *status_label;
    QPushButton *start_stop_button;
    QPushButton *open_folder_button;
    QPlainTextEdit *editor;
    QGroupBox *settings_box;
    QLineEdit *endpoint_edit;
    QLineEdit *key_edit;
    QLineEdit *model_edit;
    QLineEdit *dir_edit;
    QLineEdit *title_edit;
    QSpinBox *interval_spin;
    QToolButton *browse_button;

    bool updating_editor = false;

    MarkdownNotesSettings read_settings() const;

    void load_settings();

    void save_settings() const;

    void set_running_ui(bool running);
};

#endif //OBS_GOOGLE_CAPTION_PLUGIN_NOTESDOCK_H
