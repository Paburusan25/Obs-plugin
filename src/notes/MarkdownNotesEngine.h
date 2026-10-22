//
// Motor de notas en Markdown organizadas por IA.
// Solo depende de Qt Core + Qt Network (nada de OBS), para poder probarlo aparte.
//

#ifndef OBS_GOOGLE_CAPTION_PLUGIN_MARKDOWNNOTESENGINE_H
#define OBS_GOOGLE_CAPTION_PLUGIN_MARKDOWNNOTESENGINE_H

#include <QObject>
#include <QString>
#include <QTimer>
#include <QByteArray>
#include <QElapsedTimer>

class QNetworkAccessManager;
class QNetworkReply;

struct MarkdownNotesSettings {
    QString endpoint;          // p. ej. https://api.openai.com/v1  (se agrega /chat/completions si falta)
    QString api_key;
    QString model;
    QString output_dir;
    QString file_basename = "reunion_notas";
    QString document_title = "Acta de la reunión";
    QString system_prompt;     // vacío = usar el prompt por defecto
    int interval_sec = 20;     // cada cuánto se manda texto nuevo a la IA
    int min_chars = 80;        // mínimo de texto nuevo acumulado antes de llamar a la IA
    int timeout_sec = 60;

    bool isUsable(QString *why = nullptr) const;
};

class MarkdownNotesEngine : public QObject {
Q_OBJECT
public:
    explicit MarkdownNotesEngine(QObject *parent = nullptr);

    ~MarkdownNotesEngine() override;

    bool start(const MarkdownNotesSettings &settings, QString *error = nullptr);

    // Procesa lo que quede pendiente y termina. Emite finished() al acabar.
    void stop();

    // Cierre inmediato sin red (p. ej. al cerrar OBS): guarda el texto pendiente en "Sin clasificar".
    void shutdownNow();

    void addText(const QString &text);

    // El usuario editó el documento a mano: la IA debe partir de esta versión.
    void setDocument(const QString &markdown);

    bool isRunning() const { return running; }

    QString outputFilePath() const { return file_path; }

    QString document() const { return doc; }

    // ---- funciones puras (probadas en tests/) ----
    static QString defaultSystemPrompt();

    static QString initialDocument(const QString &title);

    static QString endpointUrl(const QString &endpoint);

    static QByteArray buildRequestBody(const MarkdownNotesSettings &s, const QString &doc, const QString &new_text);

    static bool extractContent(const QByteArray &reply, QString &out, QString &error);

    static QString stripCodeFence(const QString &text);

    static bool plausibleDocument(const QString &old_doc, const QString &new_doc);

    static QString appendUnclassified(const QString &doc, const QString &text);

signals:

    void document_updated(const QString &markdown);

    void status_changed(const QString &text);

    void finished();

private slots:

    void on_timer();

    void on_reply_finished();

private:
    MarkdownNotesSettings settings;
    QNetworkAccessManager *nam = nullptr;
    QNetworkReply *reply = nullptr;
    QTimer timer;

    bool running = false;
    bool stopping = false;
    bool stop_flush_tried = false;

    QString doc;
    QString pending;
    QElapsedTimer pending_age;   // desde cuándo hay texto esperando
    QString in_flight_text;
    quint64 doc_revision = 0;
    quint64 in_flight_revision = 0;
    int consecutive_failures = 0;
    QString file_path;

    void maybe_send(bool force);

    void send_request();

    bool write_file();

    void set_document_internal(const QString &markdown);

    void finish_stop();

    void handle_failure(const QString &why);
};

#endif //OBS_GOOGLE_CAPTION_PLUGIN_MARKDOWNNOTESENGINE_H
