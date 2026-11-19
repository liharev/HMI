#include "diagnostics.h"
#include "appcontroller.h"
#include "player.h"
#include "vmswindow.h"

#include <QClipboard>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QRegularExpression>
#include <QThread>

#include <glib.h>
#include <unistd.h>

namespace {

QByteArray readFile(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return f.readAll().trimmed();
}

qint64 readInt(const QString &path, qint64 def = -1)
{
    bool ok = false;
    const qint64 v = readFile(path).toLongLong(&ok);
    return ok ? v : def;
}

int worse(int a, int b) { return qMax(a, b); }

// value >= bad -> 2, >= warn -> 1
int rate(qreal value, qreal warn, qreal bad)
{
    return value >= bad ? 2 : value >= warn ? 1 : 0;
}

QVariantMap kv(const QString &k, const QString &v)
{
    return {{QStringLiteral("k"), k}, {QStringLiteral("v"), v}};
}

QVariantMap bar(const QString &label, qreal fraction, int status, const QString &text)
{
    return {{QStringLiteral("label"), label}, {QStringLiteral("fraction"), qBound<qreal>(0, fraction, 1)},
            {QStringLiteral("status"), status}, {QStringLiteral("text"), text}};
}

QVariantMap segment(int count, const QString &color, const QString &label)
{
    return {{QStringLiteral("count"), count}, {QStringLiteral("color"), color}, {QStringLiteral("label"), label}};
}

QString pct(qreal f) { return QString::number(qRound(f * 100)) + QLatin1Char('%'); }
QString mbps(qreal v) { return QString::number(v, 'f', v < 10 ? 1 : 0) + QStringLiteral(" Мбит/с"); }
QString gib(qint64 kb) { return QString::number(kb / 1048576.0, 'f', 1) + QStringLiteral(" ГБ"); }

} // namespace

Diagnostics::Diagnostics(AppController *app)
    : QObject(app)
    , m_app(app)
{
    m_timer.setInterval(2000);
    connect(&m_timer, &QTimer::timeout, this, &Diagnostics::refresh);
    m_timer.start();
    QMetaObject::invokeMethod(this, &Diagnostics::refresh, Qt::QueuedConnection);
}

void Diagnostics::refresh()
{
    const qint64 now = g_get_monotonic_time();
    m_dt = m_lastUs > 0 ? qMax(0.2, qreal(now - m_lastUs) / 1e6) : 2.0;
    m_lastUs = now;

    QVariantList s;
    s << streamsSection();      // first: provides the bitrate for the network
    s << cpuSection();
    s << temperatureSection();
    s << memorySection();
    s << networkSection();
    s << decoderSection();
    s << gpuSection();

    int status = 0;
    QStringList bad, warn;
    for (const QVariant &v : s) {
        const QVariantMap m = v.toMap();
        const int st = m.value(QStringLiteral("status")).toInt();
        status = worse(status, st);
        if (st == 2)
            bad << m.value(QStringLiteral("title")).toString();
        else if (st == 1)
            warn << m.value(QStringLiteral("title")).toString();
    }
    m_sections = s;
    m_status = status;
    if (!bad.isEmpty())
        m_summary = QStringLiteral("Серьёзные проблемы: ") + bad.join(QStringLiteral(", "))
                    + (warn.isEmpty() ? QString() : QStringLiteral("; замечания: ") + warn.join(QStringLiteral(", ")));
    else if (!warn.isEmpty())
        m_summary = QStringLiteral("Есть замечания: ") + warn.join(QStringLiteral(", "));
    else
        m_summary = QStringLiteral("Всё в порядке");
    m_updatedAt = QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss"));
    Q_EMIT updated();
}

// ---------------------------------------------------------------------------
QVariantMap Diagnostics::streamsSection()
{
    const QList<Player *> players = m_app->allPlayers();
    int good = 0, warnN = 0, badN = 0, off = 0;
    qreal shown = 0, nominal = 0, bitrate = 0;
    QVariantList problems;
    bool display = false, decoder = false, source = false, noVideo = false;
    for (Player *p : players) {
        switch (p->health()) {
        case Player::HealthGood: ++good; break;
        case Player::HealthWarning: ++warnN; break;
        case Player::HealthBad: ++badN; break;
        default: ++off; break;
        }
        if (p->status() == Player::Playing) {
            shown += p->shownFps();
            nominal += p->nominalFps() > 0 ? p->nominalFps() : p->fps();
            bitrate += p->bitrate();
        }
        if (p->health() >= Player::HealthWarning) {
            const QString t = p->healthText().section(QLatin1Char('\n'), 0, 0);
            if (problems.size() < 8)
                problems << kv(p->name(), t);
            display |= t.contains(QStringLiteral("Отображение"));
            decoder |= t.contains(QStringLiteral("Декодер"));
            source |= t.contains(QStringLiteral("Источник")) || t.contains(QStringLiteral("Перебои"));
            noVideo |= t.contains(QStringLiteral("Нет видео")) || t.contains(QStringLiteral("Нет новых"));
        }
    }
    m_streamsMbps = bitrate;
    m_streamsTotal = players.size();

    int st = badN > 0 ? 2 : warnN > 0 ? 1 : 0;
    QStringList advice;
    if (noVideo)
        advice << QStringLiteral("Нет видео: проверьте адрес потока, логин/пароль и доступность камеры по сети "
                                 "(подсказка на плеере называет причину).");
    if (source)
        advice << QStringLiteral("Мало кадров от источника: проверьте сеть (раздел «Сеть»), для RTSP попробуйте "
                                 "транспорт TCP, уменьшите битрейт или используйте дополнительный поток камеры.");
    if (decoder)
        advice << QStringLiteral("Декодер не успевает: см. раздел «Видеодекодер».");
    if (display)
        advice << QStringLiteral("Отображение не успевает: см. раздел «Графика».");

    QVariantList segs;
    segs << segment(good, QStringLiteral("#22c55e"), QStringLiteral("норма"))
         << segment(warnN, QStringLiteral("#eab308"), QStringLiteral("замечания"))
         << segment(badN, QStringLiteral("#ef4444"), QStringLiteral("проблемы"))
         << segment(off, QStringLiteral("#6b7280"), QStringLiteral("остановлены"));

    return {
        {QStringLiteral("id"), QStringLiteral("streams")},
        {QStringLiteral("title"), QStringLiteral("Видеопотоки")},
        {QStringLiteral("icon"), QStringLiteral("▣")},
        {QStringLiteral("status"), players.isEmpty() ? 0 : st},
        {QStringLiteral("headline"), players.isEmpty() ? QStringLiteral("нет плееров")
                                                       : QStringLiteral("%1 из %2 в норме").arg(good).arg(players.size())},
        {QStringLiteral("fraction"), nominal > 0 ? qMin<qreal>(1, shown / nominal) : -1},
        {QStringLiteral("gaugeText"), nominal > 0 ? pct(shown / nominal) : QString()},
        {QStringLiteral("gaugeLabel"), QStringLiteral("кадров показано")},
        {QStringLiteral("segments"), segs},
        {QStringLiteral("details"), QVariantList{
             kv(QStringLiteral("Показ, к/с (всего)"), QStringLiteral("%1 из %2").arg(qRound(shown)).arg(qRound(nominal))),
             kv(QStringLiteral("Входящий видеотрафик"), mbps(bitrate))}},
        {QStringLiteral("problems"), problems},
        {QStringLiteral("advice"), advice},
    };
}

QVariantMap Diagnostics::cpuSection()
{
    QList<Cpu> cur;
    const QList<QByteArray> lines = readFile(QStringLiteral("/proc/stat")).split('\n');
    for (const QByteArray &l : lines) {
        if (!l.startsWith("cpu"))
            break;
        const QList<QByteArray> v = l.simplified().split(' ');
        Cpu c;
        for (int i = 1; i < v.size(); ++i)
            c.total += v[i].toULongLong();
        if (v.size() > 5)
            c.idle = v[4].toULongLong() + v[5].toULongLong();
        cur << c;
    }
    QList<qreal> usage;
    for (int i = 0; i < cur.size(); ++i) {
        qreal u = 0;
        if (i < m_cpuPrev.size()) {
            const quint64 dt = cur[i].total - m_cpuPrev[i].total;
            const quint64 di = cur[i].idle - m_cpuPrev[i].idle;
            u = dt ? 1.0 - qreal(di) / dt : 0;
        }
        usage << qBound<qreal>(0, u, 1);
    }
    m_cpuPrev = cur;
    const qreal total = usage.value(0);

    QVariantList bars;
    bool throttled = false;
    qreal maxCore = 0;
    for (int i = 1; i < usage.size(); ++i) {
        const int n = i - 1;
        const QString base = QStringLiteral("/sys/devices/system/cpu/cpu%1/cpufreq/").arg(n);
        const qint64 curF = readInt(base + QStringLiteral("scaling_cur_freq"));
        const qint64 maxF = readInt(base + QStringLiteral("cpuinfo_max_freq"));
        QString text = pct(usage[i]);
        if (curF > 0)
            text += QStringLiteral(" · %1 ГГц").arg(curF / 1e6, 0, 'f', 1);
        if (usage[i] > 0.6 && curF > 0 && maxF > 0 && curF < 0.7 * maxF)
            throttled = true;
        maxCore = qMax(maxCore, usage[i]);
        bars << bar(QStringLiteral("CPU%1").arg(n), usage[i], rate(usage[i], 0.8, 0.95), text);
    }

    int st = rate(total, 0.65, 0.85);
    QStringList advice;
    if (st == 2)
        advice << QStringLiteral("Процессор перегружен: включите аппаратное декодирование (vendor-ядро + MPP), "
                                 "используйте дополнительные потоки камер с меньшим разрешением, уменьшите число плееров.");
    else if (st == 1)
        advice << QStringLiteral("Высокая загрузка процессора: проверьте, сколько потоков декодируется программно "
                                 "(раздел «Видеодекодер»).");
    if (maxCore > 0.95 && total < 0.5) {
        st = worse(st, 1);
        advice << QStringLiteral("Одно ядро загружено полностью при свободных остальных: какая-то операция идёт в один "
                                 "поток (например, программное декодирование крупного потока или отрисовка окна).");
    }
    if (throttled) {
        st = worse(st, 1);
        advice << QStringLiteral("Частота ядер снижена под нагрузкой (троттлинг): проверьте охлаждение и питание "
                                 "(блок питания 5 В/4 А или PD 20 Вт+).");
    }

    const QList<QByteArray> la = readFile(QStringLiteral("/proc/loadavg")).split(' ');
    return {
        {QStringLiteral("id"), QStringLiteral("cpu")},
        {QStringLiteral("title"), QStringLiteral("Процессор")},
        {QStringLiteral("icon"), QStringLiteral("⚙")},
        {QStringLiteral("status"), st},
        {QStringLiteral("headline"), pct(total)},
        {QStringLiteral("fraction"), total},
        {QStringLiteral("gaugeText"), pct(total)},
        {QStringLiteral("gaugeLabel"), QStringLiteral("загрузка")},
        {QStringLiteral("bars"), bars},
        {QStringLiteral("details"), QVariantList{
             kv(QStringLiteral("Ядер"), QString::number(qMax(0, int(usage.size()) - 1))),
             kv(QStringLiteral("Средняя нагрузка (1/5/15 мин)"),
                la.size() >= 3 ? QStringLiteral("%1 / %2 / %3").arg(QString::fromLatin1(la[0]), QString::fromLatin1(la[1]),
                                                                     QString::fromLatin1(la[2]))
                               : QStringLiteral("—"))}},
        {QStringLiteral("advice"), advice},
    };
}

QVariantMap Diagnostics::temperatureSection()
{
    QVariantList bars;
    qreal maxT = -1;
    const QDir dir(QStringLiteral("/sys/class/thermal"));
    for (const QString &z : dir.entryList({QStringLiteral("thermal_zone*")}, QDir::Dirs)) {
        const qint64 t = readInt(dir.filePath(z + QStringLiteral("/temp")));
        if (t <= 0)
            continue;
        const qreal c = t / 1000.0;
        QString type = QString::fromLatin1(readFile(dir.filePath(z + QStringLiteral("/type"))));
        type.remove(QStringLiteral("-thermal")).remove(QStringLiteral("_thermal"));
        maxT = qMax(maxT, c);
        bars << bar(type, c / 100.0, rate(c, 75, 85), QStringLiteral("%1 °C").arg(c, 0, 'f', 0));
    }
    int st = maxT < 0 ? -1 : rate(maxT, 75, 85);
    QStringList advice;
    if (st == 2)
        advice << QStringLiteral("Перегрев: RK3588 снижает частоты при ~85 °C. Установите радиатор с вентилятором, "
                                 "обеспечьте приток воздуха в корпусе.");
    else if (st == 1)
        advice << QStringLiteral("Температура высокая: при полной нагрузке возможен троттлинг. Рекомендуется активное охлаждение.");
    return {
        {QStringLiteral("id"), QStringLiteral("temp")},
        {QStringLiteral("title"), QStringLiteral("Температура")},
        {QStringLiteral("icon"), QStringLiteral("🌡")},
        {QStringLiteral("status"), st},
        {QStringLiteral("headline"), maxT < 0 ? QStringLiteral("нет данных") : QStringLiteral("%1 °C").arg(maxT, 0, 'f', 0)},
        {QStringLiteral("fraction"), maxT < 0 ? -1 : maxT / 100.0},
        {QStringLiteral("gaugeText"), maxT < 0 ? QString() : QStringLiteral("%1°").arg(maxT, 0, 'f', 0)},
        {QStringLiteral("gaugeLabel"), QStringLiteral("максимум")},
        {QStringLiteral("bars"), bars},
        {QStringLiteral("advice"), advice},
    };
}

QVariantMap Diagnostics::memorySection()
{
    QHash<QString, qint64> mi;
    for (const QByteArray &l : readFile(QStringLiteral("/proc/meminfo")).split('\n')) {
        const int c = l.indexOf(':');
        if (c > 0)
            mi.insert(QString::fromLatin1(l.left(c)), l.mid(c + 1).simplified().split(' ').value(0).toLongLong());
    }
    qint64 rss = 0;
    for (const QByteArray &l : readFile(QStringLiteral("/proc/self/status")).split('\n'))
        if (l.startsWith("VmRSS:"))
            rss = l.mid(6).simplified().split(' ').value(0).toLongLong();

    const qint64 total = mi.value(QStringLiteral("MemTotal"));
    const qint64 avail = mi.value(QStringLiteral("MemAvailable"));
    const qreal used = total > 0 ? 1.0 - qreal(avail) / total : 0;
    const qint64 swapT = mi.value(QStringLiteral("SwapTotal"));
    const qint64 swapF = mi.value(QStringLiteral("SwapFree"));
    const qint64 cmaT = mi.value(QStringLiteral("CmaTotal"));
    const qint64 cmaF = mi.value(QStringLiteral("CmaFree"));

    int st = rate(used, 0.8, 0.9);
    QStringList advice;
    if (st > 0)
        advice << QStringLiteral("Мало свободной памяти: закройте лишние программы, уменьшите число плееров "
                                 "или используйте потоки с меньшим разрешением.");
    if (swapT > 0 && swapT - swapF > swapT / 4) {
        st = worse(st, 1);
        advice << QStringLiteral("Активно используется файл подкачки — это замедляет работу.");
    }
    QVariantList bars;
    bars << bar(QStringLiteral("ОЗУ"), used, rate(used, 0.8, 0.9),
                QStringLiteral("%1 из %2").arg(gib(total - avail), gib(total)));
    if (cmaT > 0) {
        const qreal cu = 1.0 - qreal(cmaF) / cmaT;
        const int cs = rate(cu, 0.85, 0.95);
        bars << bar(QStringLiteral("CMA (видеобуферы)"), cu, cs,
                    QStringLiteral("%1 из %2 МБ").arg((cmaT - cmaF) / 1024).arg(cmaT / 1024));
        if (cs > 0) {
            st = worse(st, cs);
            advice << QStringLiteral("Заканчивается CMA-память для видеобуферов декодера: увеличьте её параметром ядра "
                                     "(в /boot/armbianEnv.txt: extraargs=cma=1024M) и перезагрузитесь.");
        }
    }
    if (swapT > 0)
        bars << bar(QStringLiteral("Подкачка"), qreal(swapT - swapF) / swapT, 0, gib(swapT - swapF));

    // Who uses the memory (only when it is tight: scanning /proc costs a bit).
    QVariantList details{kv(QStringLiteral("Использует CAMAI VMS"), QStringLiteral("%1 МБ").arg(rss / 1024))};
    if (st > 0) {
        QList<QPair<qint64, QString>> procs;
        QHash<QString, qint64> byName;
        const QDir proc(QStringLiteral("/proc"));
        for (const QString &pid : proc.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            if (!pid.at(0).isDigit())
                continue;
            QString name;
            qint64 r = 0;
            for (const QByteArray &l : readFile(proc.filePath(pid + QStringLiteral("/status"))).split('\n')) {
                if (l.startsWith("Name:"))
                    name = QString::fromUtf8(l.mid(5).trimmed());
                else if (l.startsWith("VmRSS:"))
                    r = l.mid(6).simplified().split(' ').value(0).toLongLong();
            }
            if (r > 0 && !name.isEmpty())
                byName[name] += r;        // sum processes of one program (browsers, Electron)
        }
        for (auto it = byName.cbegin(); it != byName.cend(); ++it)
            procs << qMakePair(it.value(), it.key());
        std::sort(procs.begin(), procs.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
        QStringList top;
        for (int i = 0; i < qMin(5, int(procs.size())); ++i)
            top << QStringLiteral("%1 — %2 МБ").arg(procs[i].second).arg(procs[i].first / 1024);
        details << kv(QStringLiteral("Больше всего памяти занимают"), top.join(QStringLiteral(", ")));
        advice << QStringLiteral("Закройте ненужные программы из списка выше (браузеры и Electron-приложения занимают "
                                 "сотни МБ). Для постоянной работы станции видеонаблюдения лучше запускать только её.");
    }

    return {
        {QStringLiteral("id"), QStringLiteral("memory")},
        {QStringLiteral("title"), QStringLiteral("Оперативная память")},
        {QStringLiteral("icon"), QStringLiteral("▤")},
        {QStringLiteral("status"), st},
        {QStringLiteral("headline"), QStringLiteral("свободно %1").arg(gib(avail))},
        {QStringLiteral("fraction"), used},
        {QStringLiteral("gaugeText"), pct(used)},
        {QStringLiteral("gaugeLabel"), QStringLiteral("занято")},
        {QStringLiteral("bars"), bars},
        {QStringLiteral("details"), details},
        {QStringLiteral("advice"), advice},
    };
}

QVariantMap Diagnostics::networkSection()
{
    const QDir dir(QStringLiteral("/sys/class/net"));
    QVariantList bars;
    QString mainIf;
    qreal mainRx = -1, mainUtil = 0;
    int mainSpeed = -1;
    bool errors = false;
    QHash<QString, Net> cur;
    for (const QString &ifc : dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (ifc == QLatin1String("lo") || !QFileInfo::exists(dir.filePath(ifc + QStringLiteral("/device"))))
            continue;   // physical interfaces only
        const QString st = dir.filePath(ifc + QStringLiteral("/statistics/"));
        Net n;
        n.rx = quint64(readInt(st + QStringLiteral("rx_bytes"), 0));
        n.tx = quint64(readInt(st + QStringLiteral("tx_bytes"), 0));
        n.errs = quint64(readInt(st + QStringLiteral("rx_errors"), 0) + readInt(st + QStringLiteral("rx_dropped"), 0)
                         + readInt(st + QStringLiteral("rx_crc_errors"), 0));
        cur.insert(ifc, n);
        const bool up = readFile(dir.filePath(ifc + QStringLiteral("/operstate"))) == "up";
        if (!up)
            continue;
        const Net prev = m_netPrev.value(ifc, n);
        const qreal rx = (n.rx - prev.rx) * 8.0 / m_dt / 1e6;
        if (n.errs > prev.errs)
            errors = true;
        const int speed = int(readInt(dir.filePath(ifc + QStringLiteral("/speed")), -1));
        const bool wifi = QFileInfo::exists(dir.filePath(ifc + QStringLiteral("/wireless")));
        const qreal util = speed > 0 ? rx / speed : 0;
        bars << bar(ifc + (wifi ? QStringLiteral(" (Wi-Fi)") : QString()), util, rate(util, 0.7, 0.9),
                    QStringLiteral("%1 · канал %2").arg(mbps(rx), speed > 0 ? QString::number(speed) + QStringLiteral(" Мбит/с")
                                                                           : QStringLiteral("—")));
        if (rx > mainRx) {
            mainRx = rx;
            mainIf = ifc;
            mainSpeed = speed;
            mainUtil = util;
        }
    }
    m_netPrev = cur;

    int st = mainIf.isEmpty() ? 2 : rate(mainUtil, 0.7, 0.9);
    QStringList advice;
    if (mainIf.isEmpty())
        advice << QStringLiteral("Нет активного сетевого подключения.");
    if (mainSpeed > 0 && mainSpeed <= 100 && m_streamsTotal > 0) {
        st = worse(st, m_streamsMbps > 0.7 * mainSpeed ? 2 : 1);
        advice << QStringLiteral("Подключение работает на %1 Мбит/с, хотя порт ROCK 5T рассчитан на 2,5 Гбит/с. Обычно "
                                 "причина — кабель с двумя парами или плохо обжатый разъём (нужна категория 5e/6, все 4 пары) "
                                 "либо порт коммутатора на 100 Мбит/с.").arg(mainSpeed);
    }
    if (mainSpeed > 0 && m_streamsMbps > 0.8 * mainSpeed) {
        st = worse(st, 2);
        advice << QStringLiteral("Потокам нужно %1, а канал — %2 Мбит/с: кадры не успевают приходить. Используйте "
                                 "дополнительные потоки камер, уменьшите битрейт на сервере или подключитесь быстрее.")
                      .arg(mbps(m_streamsMbps)).arg(mainSpeed);
    } else if (st >= 1 && mainUtil >= 0.7) {
        advice << QStringLiteral("Канал загружен более чем на 70%: при пиках трафика возможны потери кадров.");
    }
    if (errors) {
        st = worse(st, 1);
        advice << QStringLiteral("На интерфейсе растут ошибки/потери пакетов: проверьте кабель и коммутатор.");
    }
    if (m_app->balanceOff())
        advice << QStringLiteral("Программа определила, что кадров не хватает из-за источника/сети, а не декодера.");

    return {
        {QStringLiteral("id"), QStringLiteral("network")},
        {QStringLiteral("title"), QStringLiteral("Сеть")},
        {QStringLiteral("icon"), QStringLiteral("⇅")},
        {QStringLiteral("status"), st},
        {QStringLiteral("headline"), mainIf.isEmpty() ? QStringLiteral("нет подключения") : mbps(qMax<qreal>(0, mainRx))},
        {QStringLiteral("fraction"), mainSpeed > 0 ? mainUtil : -1},
        {QStringLiteral("gaugeText"), mainSpeed > 0 ? pct(mainUtil) : QString()},
        {QStringLiteral("gaugeLabel"), QStringLiteral("загрузка канала")},
        {QStringLiteral("bars"), bars},
        {QStringLiteral("details"), QVariantList{
             kv(QStringLiteral("Основной интерфейс"), mainIf.isEmpty() ? QStringLiteral("—") : mainIf),
             kv(QStringLiteral("Скорость подключения"),
                mainSpeed > 0 ? QString::number(mainSpeed) + QStringLiteral(" Мбит/с") : QStringLiteral("—")),
             kv(QStringLiteral("Нужно видеопотокам"), mbps(m_streamsMbps))}},
        {QStringLiteral("advice"), advice},
    };
}

QVariantMap Diagnostics::decoderSection()
{
    const QList<Player *> players = m_app->allPlayers();
    int hw = 0, sw = 0, moved = 0, lagging = 0, hevcSw = 0, forcedSw = 0, fallbackSw = 0;
    QHash<QString, int> byDecoder;
    for (Player *p : players) {
        if (p->status() != Player::Playing || p->decoderName().isEmpty())
            continue;
        byDecoder[p->decoderName()]++;
        if (p->hardwareDecoding()) {
            ++hw;
            if (p->decoderLagging())
                ++lagging;
        } else {
            ++sw;
            if (p->balancedSoftware())
                ++moved;
            else if (p->decoder() == Player::DecoderSoftware)
                ++forcedSw;
            else if (p->decoder() == Player::DecoderAuto && p->codec() != QLatin1String("RAW"))
                ++fallbackSw;
            if (p->codec() == QLatin1String("H.265"))
                ++hevcSw;
        }
    }
    const QString hwList = m_app->hardwareDecoders();
    const bool hasHw = !hwList.startsWith(QStringLiteral("нет"));
    const bool hasMpp = hwList.contains(QStringLiteral("mpp"));
    const QString kernel = QString::fromLatin1(readFile(QStringLiteral("/proc/sys/kernel/osrelease")));

    int st = 0;
    QStringList advice;
    if (!hasHw && sw > 0) {
        st = sw > 4 ? 2 : 1;
        advice << QStringLiteral("Нет аппаратного декодера: все потоки декодирует процессор. Для RK3588 установите "
                                 "vendor-ядро и MPP (scripts/build-rockchip-mpp.sh) — до 32 потоков 1080p.");
    }
    if (hasHw && !hasMpp && hw + sw > 4)
        advice << QStringLiteral("Используется декодер стандартного ядра (V4L2): его пропускная способность ограничена "
                                 "(≈60 кадров 1080p/с). Максимум даёт vendor-ядро с MPP.");
    if (lagging * 2 >= qMax(2, hw) && hw >= 2 && !m_app->balanceOff()) {
        st = worse(st, 1);
        advice << QStringLiteral("Аппаратный декодер не успевает: программа переносит часть потоков на процессор "
                                 "(для плееров с декодером «Авто»).");
    }
    if (hasHw && sw > 0 && hw == 0 && sw > 2) {
        st = worse(st, 2);
        advice << QStringLiteral("Аппаратный декодер есть, но все %1 поток(ов) декодирует процессор: это сильно нагружает "
                                 "CPU и вывод.").arg(sw);
    } else if (hasHw && sw > hw && sw > 2) {
        st = worse(st, 1);
    }
    if (moved > 0)
        advice << QStringLiteral("%1 поток(ов) перенесено на процессор автоматическим распределением. Если аппаратный декодер "
                                 "свободен, они вернутся сами (по одному каждые ~10 с).").arg(moved);
    if (forcedSw > 0 && hasHw)
        advice << QStringLiteral("%1 плеер(ов) настроены «Декодер: Только программный» — переключите на «Авто» в "
                                 "настройках плеера.").arg(forcedSw);
    if (fallbackSw > 0 && hasHw)
        advice << QStringLiteral("%1 поток(ов) с декодером «Авто» перешли на программный после сбоя аппаратного декодера. "
                                 "Чтобы проверить заново, в настройках плеера выберите другой декодер и снова «Авто».")
                      .arg(fallbackSw);
    if (hevcSw > 0 && !hasMpp) {
        st = worse(st, 1);
        advice << QStringLiteral("%1 поток(ов) H.265 декодируются процессором: аппаратного декодера H.265 нет "
                                 "(в стандартном ядре). Поможет vendor-ядро с MPP.").arg(hevcSw);
    }

    QVariantList segs;
    segs << segment(hw, QStringLiteral("#22c55e"), QStringLiteral("аппаратно"))
         << segment(sw - moved, QStringLiteral("#3b82f6"), QStringLiteral("программно"))
         << segment(moved, QStringLiteral("#a855f7"), QStringLiteral("перенесено на CPU"));
    QStringList dec;
    for (auto it = byDecoder.cbegin(); it != byDecoder.cend(); ++it)
        dec << QStringLiteral("%1 × %2").arg(it.key()).arg(it.value());

    return {
        {QStringLiteral("id"), QStringLiteral("decoder")},
        {QStringLiteral("title"), QStringLiteral("Видеодекодер")},
        {QStringLiteral("icon"), QStringLiteral("▶")},
        {QStringLiteral("status"), st},
        {QStringLiteral("headline"), hasMpp ? QStringLiteral("Rockchip MPP")
                                     : hasHw ? QStringLiteral("V4L2 (стандартное ядро)")
                                             : QStringLiteral("только программный")},
        {QStringLiteral("fraction"), hw + sw > 0 ? qreal(hw) / (hw + sw) : -1},
        {QStringLiteral("gaugeText"), hw + sw > 0 ? pct(qreal(hw) / (hw + sw)) : QString()},
        {QStringLiteral("gaugeLabel"), QStringLiteral("аппаратно")},
        {QStringLiteral("segments"), segs},
        {QStringLiteral("details"), QVariantList{
             kv(QStringLiteral("Аппаратные декодеры"), hwList),
             kv(QStringLiteral("Декодеры потоков"), dec.isEmpty() ? QStringLiteral("—") : dec.join(QStringLiteral(", "))),
             kv(QStringLiteral("Ядро Linux"), kernel),
             kv(QStringLiteral("Распределение"), m_app->decoderBalance().isEmpty() ? QStringLiteral("—")
                                                                                   : m_app->decoderBalance())}},
        {QStringLiteral("advice"), advice},
    };
}

QVariantMap Diagnostics::gpuSection()
{
    // devfreq "load" looks like "34@800000000Hz".
    qreal load = -1;
    qint64 curF = -1;
    const QDir df(QStringLiteral("/sys/class/devfreq"));
    for (const QString &d : df.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (!d.contains(QStringLiteral("gpu")))
            continue;
        const QByteArray l = readFile(df.filePath(d + QStringLiteral("/load")));
        bool ok = false;
        const int v = l.split('@').value(0).toInt(&ok);
        if (ok)
            load = v / 100.0;
        curF = readInt(df.filePath(d + QStringLiteral("/cur_freq")));
    }
    const bool soft = m_app->softwareRendering();
    int glZero = 0, glCopy = 0, qt = 0;
    for (Player *p : m_app->allPlayers()) {
        if (p->status() != Player::Playing)
            continue;
        const QString path = p->renderPath();
        if (path.contains(QStringLiteral("Qt")))
            ++qt;
        else if (path.contains(QStringLiteral("копир")))
            ++glCopy;
        else
            ++glZero;
    }
    QVariantList bars;
    int slowWindows = 0;
    for (QObject *o : m_app->windowsList()) {
        auto *w = qobject_cast<VmsWindow *>(o);
        if (!w || !w->isOpen() || w->renderFps() < 0)
            continue;
        const qreal fps = w->renderFps();
        const int ws = (w->playerCount() > 0 && fps < 8) ? 1 : 0;
        slowWindows += ws;
        bars << bar(w->title(), qMin<qreal>(1, fps / 60.0), ws, QStringLiteral("%1 к/с").arg(fps, 0, 'f', 0));
    }

    int st = soft ? 2 : load >= 0 ? rate(load, 0.75, 0.9) : 0;
    QStringList advice;
    if (soft)
        advice << QStringLiteral("Интерфейс рисуется процессором (%1): нет драйвера видеочипа. Включите Panfrost/Panthor "
                                 "(для vendor-ядра — оверлей panthor-gpu в armbian-config) или установите libmali.")
                      .arg(m_app->glRenderer());
    if (load >= 0.75)
        advice << QStringLiteral("Видеочип сильно загружен: уменьшите число одновременно видимых плееров или разрешение "
                                 "потоков; обои и анимации тоже потребляют ресурсы.");
    if (glCopy + qt > 0) {
        st = worse(st, 1);
        advice << QStringLiteral("%1 поток(ов) выводятся запасным способом (копирование или Qt Multimedia): прямая "
                                 "передача кадров в видеочип на этом сочетании драйверов работает медленно.").arg(glCopy + qt);
    }
    if (slowWindows > 0 && !soft)
        advice << QStringLiteral("Окно перерисовывается реже 8 раз в секунду при работающих плеерах — см. подсказки на плеерах.");

    QString headline = soft ? QStringLiteral("без аппаратного ускорения") : m_app->glRenderer().section(QStringLiteral(" — "), 0, 0);
    if (headline.isEmpty())
        headline = QStringLiteral("—");
    return {
        {QStringLiteral("id"), QStringLiteral("gpu")},
        {QStringLiteral("title"), QStringLiteral("Графика")},
        {QStringLiteral("icon"), QStringLiteral("◧")},
        {QStringLiteral("status"), st},
        {QStringLiteral("headline"), headline},
        {QStringLiteral("fraction"), load},
        {QStringLiteral("gaugeText"), load >= 0 ? pct(load) : QString()},
        {QStringLiteral("gaugeLabel"), QStringLiteral("загрузка GPU")},
        {QStringLiteral("bars"), bars},
        {QStringLiteral("details"), QVariantList{
             kv(QStringLiteral("Видеочип"), m_app->glRenderer().isEmpty() ? QStringLiteral("—") : m_app->glRenderer()),
             kv(QStringLiteral("Частота GPU"), curF > 0 ? QStringLiteral("%1 МГц").arg(curF / 1000000) : QStringLiteral("—")),
             kv(QStringLiteral("Вывод видео"), m_app->renderBackend()),
             kv(QStringLiteral("Способ вывода потоков"),
                QStringLiteral("GPU напрямую: %1, с копированием: %2, Qt Multimedia: %3").arg(glZero).arg(glCopy).arg(qt))}},
        {QStringLiteral("advice"), advice},
    };
}

// ---------------------------------------------------------------------------
QString Diagnostics::report() const
{
    static const char *const names[] = {"норма", "замечания", "ПРОБЛЕМЫ"};
    QStringList out;
    out << QStringLiteral("CAMAI VMS %1 — диагностика %2").arg(m_app->version(),
                                                               QDateTime::currentDateTime().toString(Qt::ISODate));
    out << QStringLiteral("Итог: %1").arg(m_summary) << QString();
    for (const QVariant &v : m_sections) {
        const QVariantMap m = v.toMap();
        const int st = m.value(QStringLiteral("status")).toInt();
        out << QStringLiteral("[%1] %2: %3").arg(st >= 0 ? QString::fromUtf8(names[st]) : QStringLiteral("—"),
                                                m.value(QStringLiteral("title")).toString(),
                                                m.value(QStringLiteral("headline")).toString());
        for (const QVariant &d : m.value(QStringLiteral("details")).toList())
            out << QStringLiteral("    %1: %2").arg(d.toMap().value(QStringLiteral("k")).toString(),
                                                   d.toMap().value(QStringLiteral("v")).toString());
        for (const QVariant &b : m.value(QStringLiteral("bars")).toList())
            out << QStringLiteral("    %1: %2").arg(b.toMap().value(QStringLiteral("label")).toString(),
                                                   b.toMap().value(QStringLiteral("text")).toString());
        for (const QVariant &p : m.value(QStringLiteral("problems")).toList())
            out << QStringLiteral("    ! %1: %2").arg(p.toMap().value(QStringLiteral("k")).toString(),
                                                     p.toMap().value(QStringLiteral("v")).toString());
        for (const QVariant &a : m.value(QStringLiteral("advice")).toList())
            out << QStringLiteral("    → %1").arg(a.toString());
        out << QString();
    }
    return out.join(QLatin1Char('\n'));
}

void Diagnostics::copyReport() const
{
    QGuiApplication::clipboard()->setText(report());
}
