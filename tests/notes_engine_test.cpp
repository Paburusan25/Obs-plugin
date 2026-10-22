// Prueba del motor MarkdownNotesEngine contra el servidor falso. Uso: notes_engine_test <puerto> <carpeta> <modo>
#include "../src/notes/MarkdownNotesEngine.h"
#include <QCoreApplication>
#include <QFile>
#include <QTimer>
#include <iostream>

static int failures = 0;
#define CHECK(c, msg) do { if (!(c)) { std::cerr << "FALLO: " << msg << "\n"; failures++; } else std::cout << "ok: " << msg << "\n"; } while (0)

static QString readFile(const QString &p) { QFile f(p); f.open(QIODevice::ReadOnly); return QString::fromUtf8(f.readAll()); }

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    const QString mode = argv[3];

    // ---- pruebas de funciones puras (no necesitan red) ----
    if (mode == "pure") {
        CHECK(MarkdownNotesEngine::endpointUrl("https://x.com/v1/") == "https://x.com/v1/chat/completions", "endpointUrl agrega /chat/completions");
        CHECK(MarkdownNotesEngine::endpointUrl("https://x.com/v1/chat/completions") == "https://x.com/v1/chat/completions", "endpointUrl no duplica");
        CHECK(MarkdownNotesEngine::stripCodeFence("```markdown\n# a\n- b\n```") == "# a\n- b", "stripCodeFence quita la valla");
        CHECK(MarkdownNotesEngine::stripCodeFence("# a") == "# a", "stripCodeFence deja texto normal");
        QString out, err;
        CHECK(!MarkdownNotesEngine::extractContent("no json", out, err), "extractContent rechaza no-JSON");
        CHECK(!MarkdownNotesEngine::extractContent("{\"error\":{\"message\":\"x\"}}", out, err) && err.contains("x"), "extractContent reporta error del servicio");
        CHECK(MarkdownNotesEngine::extractContent("{\"choices\":[{\"message\":{\"content\":\"# t\"}}]}", out, err) && out == "# t", "extractContent lee el contenido");
        CHECK(!MarkdownNotesEngine::plausibleDocument(QString(500, 'a').prepend("# "), "# corto"), "plausible rechaza documento que pierde >20%");
        CHECK(!MarkdownNotesEngine::plausibleDocument(MarkdownNotesEngine::initialDocument("Acta"), "# solo"), "plausible rechaza respuesta truncada en documento chico");
        CHECK(!MarkdownNotesEngine::plausibleDocument("# a", "texto sin encabezado"), "plausible exige encabezado");
        CHECK(MarkdownNotesEngine::plausibleDocument("# a", "# b\n- c"), "plausible acepta documento válido");
        QString d = MarkdownNotesEngine::appendUnclassified("# T\n\n## Sin clasificar\n- uno\n\n## Otra\n- x\n", "dos\ntres");
        CHECK(d.indexOf("- dos") > d.indexOf("- uno") && d.indexOf("- tres") < d.indexOf("## Otra"), "appendUnclassified inserta dentro de la sección");
        QString d2 = MarkdownNotesEngine::appendUnclassified("# T\n", "x");
        CHECK(d2.contains("## Sin clasificar") && d2.contains("- x"), "appendUnclassified crea la sección si falta");
        MarkdownNotesSettings s; s.model = "m"; s.output_dir = "/tmp"; s.endpoint = "";
        CHECK(!s.isUsable(), "settings sin endpoint no son usables");
        return failures ? 1 : 0;
    }

    MarkdownNotesSettings s;
    s.endpoint = QString("http://127.0.0.1:%1/v1").arg(argv[1]);
    s.api_key = "clave-test"; s.model = "modelo-test"; s.output_dir = argv[2];
    s.interval_sec = 5; s.min_chars = 10; s.timeout_sec = 5;

    if (mode == "short") {
        // poco texto (< min_chars): debe procesarse igual tras 3 intervalos, sin esperar al stop()
        s.min_chars = 100000;
        MarkdownNotesEngine e2;
        int updates = 0;
        QObject::connect(&e2, &MarkdownNotesEngine::document_updated, &app, [&]() { updates++; });
        QString er2;
        CHECK(e2.start(s, &er2), "start() modo short");
        e2.addText("una sola frase corta");
        QTimer::singleShot(23000, &app, [&]() {
            CHECK(updates >= 2, "frase corta procesada tras 3 intervalos sin llegar a min_chars");
            e2.shutdownNow();
            app.quit();
        });
        app.exec();
        return failures ? 1 : 0;
    }

    MarkdownNotesEngine eng;
    QString err;
    CHECK(eng.start(s, &err), "start()");
    QObject::connect(&eng, &MarkdownNotesEngine::finished, &app, [&]() {
        const QString file = readFile(eng.outputFilePath());
        std::cout << "---- archivo final ----\n" << file.toStdString() << "-----------------------\n";
        if (mode == "ok" || mode == "fence") {
            CHECK(file.contains("## Unidad de competencia: Prueba"), "la IA organizó el contenido");
            CHECK(file.contains("hola equipo revisamos el plan"), "el texto nuevo está en el .md");
            CHECK(!file.contains("```"), "sin valla de código en el archivo");
        } else {  // error500 / truncated: nada se pierde
            CHECK(file.contains("- hola equipo revisamos el plan"), "si la IA falla el texto se guarda en 'Sin clasificar'");
            CHECK(file.contains("- segunda frase"), "también la frase pendiente");
        }
        app.quit();
    });
    eng.addText("hola equipo revisamos el plan");
    QTimer::singleShot(6500, &eng, [&]() { eng.addText("segunda frase"); });
    QTimer::singleShot(8000, &eng, [&]() { eng.stop(); });
    QTimer::singleShot(40000, &app, [&]() { std::cerr << "TIMEOUT\n"; failures++; app.exit(2); });
    int rc = app.exec();
    return failures ? 1 : rc;
}
