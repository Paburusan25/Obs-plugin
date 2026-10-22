#include "MarkdownNotesEngine.h"

#include <QDateTime>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSaveFile>
#include <QUrl>

static const char *UNCLASSIFIED_HEADING = "## Sin clasificar";

bool MarkdownNotesSettings::isUsable(QString *why) const {
    auto fail = [&](const QString &m) {
        if (why)
            *why = m;
        return false;
    };
    const QUrl url(MarkdownNotesEngine::endpointUrl(endpoint));
    if (endpoint.trimmed().isEmpty() || !url.isValid() || url.scheme().isEmpty())
        return fail("Falta una URL válida del servicio de IA (por ejemplo https://api.openai.com/v1).");
    if (model.trimmed().isEmpty())
        return fail("Falta el nombre del modelo.");
    if (output_dir.trimmed().isEmpty())
        return fail("Falta la carpeta donde guardar el archivo .md.");
    if (interval_sec < 5)
        return fail("El intervalo mínimo es de 5 segundos.");
    return true;
}

// ---------------------------------------------------------------------------
// Funciones puras
// ---------------------------------------------------------------------------

QString MarkdownNotesEngine::defaultSystemPrompt() {
    return QString::fromUtf8(
            "Eres un asistente que toma notas de una reunión en tiempo real y las organiza en un documento Markdown.\n"
            "Recibirás el DOCUMENTO ACTUAL y una NUEVA TRANSCRIPCIÓN (texto reconocido por voz: puede tener errores, "
            "falta de puntuación y frases cortadas).\n"
            "\n"
            "Tu tarea: integrar la nueva transcripción en el documento y devolver el documento COMPLETO actualizado.\n"
            "\n"
            "Estructura obligatoria (norma de competencias laborales):\n"
            "# <título de la reunión>\n"
            "## Unidad de competencia: <nombre>\n"
            "### Criterios de desempeño\n"
            "- <criterio>\n"
            "### Resultados de aprendizaje\n"
            "- <resultado>\n"
            "### Criterios de evaluación\n"
            "- <criterio>\n"
            "## Sin clasificar\n"
            "- <ideas que todavía no encajan en ninguna unidad de competencia>\n"
            "\n"
            "Reglas:\n"
            "1. Mantén y respeta todo lo que ya está en el documento; el usuario puede haberlo corregido a mano. "
            "No borres contenido salvo para fusionar duplicados.\n"
            "2. Crea una nueva sección \"Unidad de competencia\" cuando se hable de un tema distinto; si el tema ya existe, "
            "agrega el contenido ahí.\n"
            "3. Clasifica cada idea como criterio de desempeño, resultado de aprendizaje o criterio de evaluación solo si "
            "la transcripción lo respalda. Si no estás seguro, ponla en \"Sin clasificar\".\n"
            "4. No inventes información que no se haya dicho. Corrige solo errores evidentes de reconocimiento de voz.\n"
            "5. Escribe en el mismo idioma de la reunión, con viñetas breves y claras.\n"
            "6. Responde ÚNICAMENTE con el documento Markdown completo, sin explicaciones y sin bloques de código.");
}

QString MarkdownNotesEngine::initialDocument(const QString &title) {
    return QString("# %1\n\n_Notas generadas en tiempo real. Inicio: %2_\n\n%3\n\n")
            .arg(title.trimmed().isEmpty() ? QString("Acta de la reunión") : title.trimmed(),
                 QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm"),
                 UNCLASSIFIED_HEADING);
}

QString MarkdownNotesEngine::endpointUrl(const QString &endpoint) {
    QString e = endpoint.trimmed();
    while (e.endsWith('/'))
        e.chop(1);
    if (e.isEmpty())
        return e;
    if (e.endsWith("/chat/completions"))
        return e;
    return e + "/chat/completions";
}

QByteArray MarkdownNotesEngine::buildRequestBody(const MarkdownNotesSettings &s, const QString &doc,
                                                 const QString &new_text) {
    const QString system = s.system_prompt.trimmed().isEmpty() ? defaultSystemPrompt() : s.system_prompt;

    const QString user = QString("DOCUMENTO ACTUAL:\n<<<\n%1\n>>>\n\nNUEVA TRANSCRIPCIÓN:\n<<<\n%2\n>>>\n\n"
                                 "Devuelve el documento Markdown completo y actualizado.")
            .arg(doc, new_text);

    QJsonArray messages;
    messages.append(QJsonObject{{"role",    "system"},
                                {"content", system}});
    messages.append(QJsonObject{{"role",    "user"},
                                {"content", user}});

    QJsonObject body{
            {"model",       s.model},
            {"messages",    messages},
            {"temperature", 0.2},
    };
    return QJsonDocument(body).toJson(QJsonDocument::Compact);
}

QString MarkdownNotesEngine::stripCodeFence(const QString &text) {
    QString t = text.trimmed();
    static const QRegularExpression fence("^```[a-zA-Z]*\\s*\\n(.*)\\n```$",
                                          QRegularExpression::DotMatchesEverythingOption);
    const auto m = fence.match(t);
    if (m.hasMatch())
        return m.captured(1).trimmed();
    return t;
}

bool MarkdownNotesEngine::extractContent(const QByteArray &reply, QString &out, QString &error) {
    QJsonParseError perr{};
    const QJsonDocument jd = QJsonDocument::fromJson(reply, &perr);
    if (perr.error != QJsonParseError::NoError || !jd.isObject()) {
        error = "La respuesta del servicio no es JSON válido.";
        return false;
    }
    const QJsonObject root = jd.object();

    if (root.contains("error")) {
        const QJsonValue ev = root.value("error");
        QString msg = ev.isObject() ? ev.toObject().value("message").toString() : ev.toString();
        error = "El servicio devolvió un error: " + (msg.isEmpty() ? QString("desconocido") : msg);
        return false;
    }

    const QJsonArray choices = root.value("choices").toArray();
    if (choices.isEmpty()) {
        error = "La respuesta no trae 'choices'.";
        return false;
    }
    const QString content = choices.at(0).toObject().value("message").toObject().value("content").toString();
    if (content.trimmed().isEmpty()) {
        error = "La IA devolvió una respuesta vacía.";
        return false;
    }
    out = stripCodeFence(content);
    return true;
}

bool MarkdownNotesEngine::plausibleDocument(const QString &old_doc, const QString &new_doc) {
    const QString n = new_doc.trimmed();
    if (n.isEmpty() || !n.startsWith('#'))
        return false;
    // Cada ronda solo agrega contenido: una respuesta que encoge el documento más de un 20 % se descarta
    // (evita que una respuesta truncada o alucinada borre las notas).
    if (n.size() < old_doc.trimmed().size() * 8 / 10)
        return false;
    return true;
}

QString MarkdownNotesEngine::appendUnclassified(const QString &doc, const QString &text) {
    QStringList items;
    for (const QString &line: text.split('\n', Qt::SkipEmptyParts)) {
        const QString l = line.trimmed();
        if (!l.isEmpty())
            items << "- " + l;
    }
    if (items.isEmpty())
        return doc;

    QString d = doc;
    const QString block = items.join('\n') + "\n";

    const int idx = d.indexOf(UNCLASSIFIED_HEADING);
    if (idx < 0) {
        if (!d.endsWith('\n'))
            d += '\n';
        return d + "\n" + UNCLASSIFIED_HEADING + "\n" + block;
    }

    // insertar al final de la sección "Sin clasificar" (hasta el siguiente encabezado "## " o el final)
    const int after_heading = d.indexOf('\n', idx);
    int end = d.size();
    if (after_heading >= 0) {
        const int next = d.indexOf(QRegularExpression("\n## "), after_heading);
        if (next >= 0)
            end = next + 1;
    }
    QString head = d.left(end);
    if (!head.endsWith('\n'))
        head += '\n';
    return head + block + d.mid(end);
}

// ---------------------------------------------------------------------------
// Motor
// ---------------------------------------------------------------------------

MarkdownNotesEngine::MarkdownNotesEngine(QObject *parent) : QObject(parent), nam(new QNetworkAccessManager(this)) {
    timer.setSingleShot(false);
    connect(&timer, &QTimer::timeout, this, &MarkdownNotesEngine::on_timer);
}

MarkdownNotesEngine::~MarkdownNotesEngine() {
    if (reply) {
        reply->disconnect(this);
        reply->abort();
        reply->deleteLater();
        reply = nullptr;
    }
}

bool MarkdownNotesEngine::start(const MarkdownNotesSettings &s, QString *error) {
    if (running) {
        if (error)
            *error = "Ya está en marcha.";
        return false;
    }
    QString why;
    if (!s.isUsable(&why)) {
        if (error)
            *error = why;
        return false;
    }

    QDir dir(s.output_dir);
    if (!dir.exists() && !dir.mkpath(".")) {
        if (error)
            *error = "No se pudo crear la carpeta: " + s.output_dir;
        return false;
    }

    settings = s;
    QString base = s.file_basename.trimmed().isEmpty() ? QString("reunion_notas") : s.file_basename.trimmed();
    base += "_" + QDateTime::currentDateTime().toString("yyyy-MM-dd_HH-mm-ss");
    file_path = dir.absoluteFilePath(base + ".md");

    pending.clear();
    in_flight_text.clear();
    consecutive_failures = 0;
    stopping = false;
    stop_flush_tried = false;
    doc_revision = 0;
    doc = initialDocument(s.document_title);

    if (!write_file()) {
        if (error)
            *error = "No se pudo escribir el archivo: " + file_path;
        return false;
    }

    running = true;
    timer.start(settings.interval_sec * 1000);
    emit document_updated(doc);
    emit status_changed("Notas IA activas → " + file_path);
    return true;
}

void MarkdownNotesEngine::addText(const QString &text) {
    if (!running || stopping)
        return;
    const QString t = text.trimmed();
    if (t.isEmpty())
        return;
    if (pending.isEmpty())
        pending_age.start();
    else
        pending += '\n';
    pending += t;
}

void MarkdownNotesEngine::setDocument(const QString &markdown) {
    if (!running)
        return;
    if (markdown == doc)
        return;
    doc = markdown;
    doc_revision++;
    write_file();
}

void MarkdownNotesEngine::set_document_internal(const QString &markdown) {
    doc = markdown;
    write_file();
    emit document_updated(doc);
}

bool MarkdownNotesEngine::write_file() {
    if (file_path.isEmpty())
        return false;
    QSaveFile f(file_path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    f.write(doc.toUtf8());
    return f.commit();
}

void MarkdownNotesEngine::on_timer() {
    maybe_send(false);
}

void MarkdownNotesEngine::maybe_send(bool force) {
    if (!running || reply)
        return;
    if (pending.isEmpty())
        return;
    // Con poco texto se espera a que se acumule, pero nunca más de 3 intervalos
    // (así una frase suelta seguida de silencio también se procesa).
    const bool too_old = pending_age.isValid() && pending_age.elapsed() > 3LL * settings.interval_sec * 1000;
    if (!force && pending.size() < settings.min_chars && !too_old)
        return;
    send_request();
}

void MarkdownNotesEngine::send_request() {
    in_flight_text = pending;
    pending.clear();
    in_flight_revision = doc_revision;

    QNetworkRequest req{QUrl(endpointUrl(settings.endpoint))};
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    if (!settings.api_key.trimmed().isEmpty())
        req.setRawHeader("Authorization", "Bearer " + settings.api_key.trimmed().toUtf8());
    req.setTransferTimeout(settings.timeout_sec * 1000);

    emit status_changed("Organizando con IA…");
    reply = nam->post(req, buildRequestBody(settings, doc, in_flight_text));
    connect(reply, &QNetworkReply::finished, this, &MarkdownNotesEngine::on_reply_finished);
}

void MarkdownNotesEngine::handle_failure(const QString &why) {
    consecutive_failures++;
    // el texto no se pierde: vuelve al inicio de la cola para el siguiente intento
    pending = in_flight_text + (pending.isEmpty() ? QString() : "\n" + pending);
    in_flight_text.clear();
    pending_age.start();
    emit status_changed(QString("Error de IA (intento %1): %2 — se reintentará, el texto está guardado.")
                                .arg(consecutive_failures)
                                .arg(why));
}

void MarkdownNotesEngine::on_reply_finished() {
    QNetworkReply *r = reply;
    reply = nullptr;
    if (!r)
        return;

    const QByteArray data = r->readAll();
    const bool net_ok = r->error() == QNetworkReply::NoError;
    const QString net_err = r->errorString();
    r->deleteLater();

    bool applied = false;
    if (!net_ok) {
        // algunos servicios devuelven el detalle del error en el cuerpo
        QString body_err, ignored;
        QString detail = extractContent(data, ignored, body_err) ? QString() : body_err;
        handle_failure(detail.isEmpty() ? net_err : net_err + " (" + detail + ")");
    } else {
        QString content, err;
        if (!extractContent(data, content, err)) {
            handle_failure(err);
        } else if (in_flight_revision != doc_revision) {
            // el usuario editó el documento mientras la IA trabajaba: descartar y reintentar con su versión
            pending = in_flight_text + (pending.isEmpty() ? QString() : "\n" + pending);
            in_flight_text.clear();
            emit status_changed("Editaste el documento; se reorganiza partiendo de tu versión…");
        } else if (!plausibleDocument(doc, content)) {
            handle_failure("la IA devolvió un documento incompleto");
        } else {
            consecutive_failures = 0;
            in_flight_text.clear();
            set_document_internal(content);
            applied = true;
            emit status_changed("Notas actualizadas " + QDateTime::currentDateTime().toString("HH:mm:ss"));
        }
    }
    Q_UNUSED(applied);

    if (stopping) {
        // un último intento con lo pendiente; si falla, se guarda en bruto
        if (!pending.isEmpty() && !stop_flush_tried) {
            stop_flush_tried = true;
            send_request();
            return;
        }
        finish_stop();
    }
}

void MarkdownNotesEngine::stop() {
    if (!running || stopping)
        return;
    stopping = true;
    timer.stop();

    if (reply)
        return; // on_reply_finished continúa el cierre

    if (!pending.isEmpty()) {
        stop_flush_tried = true;
        send_request();
        return;
    }
    finish_stop();
}

void MarkdownNotesEngine::shutdownNow() {
    if (!running)
        return;
    timer.stop();
    if (reply) {
        reply->disconnect(this);
        reply->abort();
        reply->deleteLater();
        reply = nullptr;
    }
    finish_stop();
}

void MarkdownNotesEngine::finish_stop() {
    QString leftover = pending;
    if (!in_flight_text.isEmpty())
        leftover = in_flight_text + (leftover.isEmpty() ? QString() : "\n" + leftover);
    if (!leftover.isEmpty()) {
        // la IA no estuvo disponible: conservar el texto sin perderlo
        set_document_internal(appendUnclassified(doc, leftover));
    }
    pending.clear();
    in_flight_text.clear();
    running = false;
    stopping = false;
    emit status_changed("Notas guardadas en " + file_path);
    emit finished();
}
