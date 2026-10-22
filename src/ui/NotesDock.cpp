#include "NotesDock.h"

#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFont>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QSettings>
#include <QSpinBox>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

#include "../CaptionPluginManager.h"
#include "../SourceCaptioner.h"
#include "log.h"

static QSettings notes_qsettings() {
    return QSettings("cloud-closed-captions", "markdown-notes");
}

NotesDock::NotesDock(CaptionPluginManager &plugin_manager, QWidget *parent)
        : QWidget(parent), plugin_manager(plugin_manager) {
    setWindowTitle("Notas IA (Markdown)");

    auto *layout = new QVBoxLayout(this);

    status_label = new QLabel("Detenido. Configura la IA y pulsa «Iniciar».", this);
    status_label->setWordWrap(true);
    layout->addWidget(status_label);

    auto *buttons = new QHBoxLayout();
    start_stop_button = new QPushButton("Iniciar notas IA", this);
    open_folder_button = new QPushButton("Abrir carpeta", this);
    buttons->addWidget(start_stop_button);
    buttons->addWidget(open_folder_button);
    layout->addLayout(buttons);

    editor = new QPlainTextEdit(this);
    editor->setPlaceholderText("Aquí aparecerá el documento Markdown organizado por la IA.\n"
                               "Puedes corregirlo a mano: la IA continuará desde tu versión.");
    QFont mono("Monospace");
    mono.setStyleHint(QFont::TypeWriter);
    editor->setFont(mono);
    editor->setMinimumHeight(220);
    layout->addWidget(editor, 1);

    settings_box = new QGroupBox("Ajustes de la IA", this);
    settings_box->setCheckable(true);
    settings_box->setChecked(false);
    auto *form = new QFormLayout();
    endpoint_edit = new QLineEdit(settings_box);
    endpoint_edit->setPlaceholderText("https://api.openai.com/v1  (cualquier API compatible con OpenAI)");
    key_edit = new QLineEdit(settings_box);
    key_edit->setEchoMode(QLineEdit::Password);
    key_edit->setPlaceholderText("Clave de API (se guarda en texto plano en la configuración de OBS)");
    model_edit = new QLineEdit(settings_box);
    model_edit->setPlaceholderText("nombre del modelo");
    title_edit = new QLineEdit(settings_box);
    dir_edit = new QLineEdit(settings_box);
    browse_button = new QToolButton(settings_box);
    browse_button->setText("…");
    auto *dir_row = new QHBoxLayout();
    dir_row->addWidget(dir_edit, 1);
    dir_row->addWidget(browse_button);
    interval_spin = new QSpinBox(settings_box);
    interval_spin->setRange(5, 600);
    interval_spin->setSuffix(" s");
    form->addRow("URL del servicio", endpoint_edit);
    form->addRow("Clave de API", key_edit);
    form->addRow("Modelo", model_edit);
    form->addRow("Título del documento", title_edit);
    form->addRow("Carpeta de salida", dir_row);
    form->addRow("Organizar cada", interval_spin);
    auto *privacy = new QLabel("El texto de la reunión se envía al servicio de IA que configures.", settings_box);
    privacy->setWordWrap(true);
    form->addRow(privacy);
    settings_box->setLayout(form);
    layout->addWidget(settings_box);

    load_settings();
    // los campos solo se ven cuando el grupo está desplegado
    auto toggle_fields = [this](bool on) {
        for (auto *w: settings_box->findChildren<QWidget *>(QString(), Qt::FindDirectChildrenOnly))
            w->setVisible(on);
    };
    connect(settings_box, &QGroupBox::toggled, this, toggle_fields);
    toggle_fields(false);

    connect(start_stop_button, &QPushButton::clicked, this, &NotesDock::on_start_stop_clicked);
    connect(open_folder_button, &QPushButton::clicked, this, &NotesDock::on_open_folder_clicked);
    connect(browse_button, &QToolButton::clicked, this, &NotesDock::on_browse_clicked);
    connect(editor, &QPlainTextEdit::textChanged, this, &NotesDock::on_editor_changed);

    connect(&engine, &MarkdownNotesEngine::document_updated, this, &NotesDock::on_document_updated);
    connect(&engine, &MarkdownNotesEngine::status_changed, this, &NotesDock::on_status);
    connect(&engine, &MarkdownNotesEngine::finished, this, &NotesDock::on_engine_finished);

    connect(&plugin_manager.source_captioner, &SourceCaptioner::caption_result_received,
            this, &NotesDock::on_caption, Qt::QueuedConnection);
}

NotesDock::~NotesDock() {
    engine.shutdownNow();
    save_settings();
}

MarkdownNotesSettings NotesDock::read_settings() const {
    MarkdownNotesSettings s;
    s.endpoint = endpoint_edit->text().trimmed();
    s.api_key = key_edit->text().trimmed();
    s.model = model_edit->text().trimmed();
    s.document_title = title_edit->text().trimmed();
    s.output_dir = dir_edit->text().trimmed();
    s.interval_sec = interval_spin->value();
    return s;
}

void NotesDock::load_settings() {
    QSettings q = notes_qsettings();
    endpoint_edit->setText(q.value("endpoint", "").toString());
    key_edit->setText(q.value("api_key", "").toString());
    model_edit->setText(q.value("model", "").toString());
    title_edit->setText(q.value("title", "Acta de la reunión").toString());
    dir_edit->setText(q.value("output_dir", QDir::homePath() + "/Documents/notas-reuniones").toString());
    interval_spin->setValue(q.value("interval_sec", 20).toInt());
}

void NotesDock::save_settings() const {
    QSettings q = notes_qsettings();
    q.setValue("endpoint", endpoint_edit->text().trimmed());
    q.setValue("api_key", key_edit->text().trimmed());
    q.setValue("model", model_edit->text().trimmed());
    q.setValue("title", title_edit->text().trimmed());
    q.setValue("output_dir", dir_edit->text().trimmed());
    q.setValue("interval_sec", interval_spin->value());
}

void NotesDock::set_running_ui(bool running) {
    start_stop_button->setText(running ? "Detener y guardar" : "Iniciar notas IA");
    settings_box->setEnabled(!running);
}

void NotesDock::on_start_stop_clicked() {
    if (engine.isRunning()) {
        start_stop_button->setEnabled(false);
        engine.stop(); // on_engine_finished reactiva el botón
        return;
    }

    save_settings();
    QString err;
    if (!engine.start(read_settings(), &err)) {
        settings_box->setChecked(true);
        on_status(err);
        return;
    }
    set_running_ui(true);
}

void NotesDock::on_engine_finished() {
    start_stop_button->setEnabled(true);
    set_running_ui(false);
}

void NotesDock::on_open_folder_clicked() {
    const QString dir = dir_edit->text().trimmed();
    if (dir.isEmpty())
        return;
    QDir().mkpath(dir);
    QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
}

void NotesDock::on_browse_clicked() {
    const QString dir = QFileDialog::getExistingDirectory(this, "Carpeta de salida", dir_edit->text());
    if (!dir.isEmpty())
        dir_edit->setText(dir);
}

void NotesDock::on_editor_changed() {
    if (updating_editor)
        return;
    engine.setDocument(editor->toPlainText());
}

void NotesDock::on_document_updated(const QString &markdown) {
    if (editor->toPlainText() == markdown)
        return;
    auto *bar = editor->verticalScrollBar();
    const int pos = bar->value();
    const bool at_end = pos >= bar->maximum() - 4;
    const QTextCursor cursor_before = editor->textCursor();
    const int cursor_pos = cursor_before.position();

    updating_editor = true;
    editor->setPlainText(markdown);
    updating_editor = false;

    QTextCursor c = editor->textCursor();
    c.setPosition(qMin(cursor_pos, markdown.size()));
    editor->setTextCursor(c);
    bar->setValue(at_end ? bar->maximum() : pos);
}

void NotesDock::on_status(const QString &text) {
    status_label->setText(text);
}

void NotesDock::on_caption(std::shared_ptr<OutputCaptionResult> caption, bool cleared, std::string) {
    if (cleared || !caption || !engine.isRunning())
        return;
    if (!caption->caption_result.final)
        return;
    engine.addText(QString::fromStdString(caption->clean_caption_text));
}
