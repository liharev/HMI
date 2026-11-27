// System diagnostics: CPU, temperature, memory, network, decoders, GPU and
// streams, each rated green / yellow / red with advice for the operator.
#pragma once

#include <QHash>
#include <QObject>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

class AppController;

class Diagnostics : public QObject
{
    Q_OBJECT
    // Overall status: 0 good, 1 minor problems, 2 serious problems.
    Q_PROPERTY(int status READ status NOTIFY updated)
    Q_PROPERTY(QString summary READ summary NOTIFY updated)
    Q_PROPERTY(QVariantList sections READ sections NOTIFY updated)
    Q_PROPERTY(QString updatedAt READ updatedAt NOTIFY updated)

public:
    explicit Diagnostics(AppController *app);

    int status() const { return m_status; }
    QString summary() const { return m_summary; }
    QVariantList sections() const { return m_sections; }
    QString updatedAt() const { return m_updatedAt; }

    // Plain-text report (copied to the clipboard for support).
    Q_INVOKABLE QString report() const;
    Q_INVOKABLE void copyReport() const;
    Q_INVOKABLE void refresh();

Q_SIGNALS:
    void updated();

private:
    struct Cpu { quint64 total = 0; quint64 idle = 0; };
    struct Net { quint64 rx = 0; quint64 tx = 0; quint64 errs = 0; };

    QVariantMap streamsSection();
    QVariantMap cpuSection();
    QVariantMap temperatureSection();
    QVariantMap memorySection();
    QVariantMap networkSection();
    QVariantMap decoderSection();
    QVariantMap gpuSection();

    AppController *m_app;
    QTimer m_timer;
    qint64 m_lastUs = 0;
    qreal m_dt = 2.0;
    QList<Cpu> m_cpuPrev;          // [0] = all cores, then per core
    QHash<QString, Net> m_netPrev;
    qint64 m_swapInPrev = -1;      // pages read back from swap (/proc/vmstat pswpin)
    int m_status = 0;
    QString m_summary;
    QVariantList m_sections;
    QString m_updatedAt;
    qreal m_streamsMbps = 0;       // bitrate the players need (for the network)
    int m_streamsTotal = 0;
};
