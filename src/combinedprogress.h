#pragma once

#include <QHash>
#include <QString>
#include <QtGlobal>

// The progress of several running instances combined into one value. The task manager keeps a single
// progress per application (the Unity LauncherEntry is keyed by the .desktop file, not by the window),
// so every instance shows the same, combined value there: the share of all bytes to write that has been
// written, each job weighted by its size. Instances that stop reporting (e.g. crashed) are dropped after
// StaleMs.
class CombinedProgress
{
public:
    static constexpr qint64 StaleMs = 5000;

    // `progress` 0..1, `totalBytes` the size of the job (a job without bytes counts as one byte).
    void set(const QString &id, double progress, qint64 totalBytes, qint64 nowMs);
    void remove(const QString &id);
    // Drops the entries not updated for more than StaleMs. Returns whether anything was dropped.
    bool expire(qint64 nowMs);
    bool isEmpty() const { return m_entries.isEmpty(); }
    qsizetype size() const { return m_entries.size(); }
    bool contains(const QString &id) const { return m_entries.contains(id); }
    // The weighted progress 0..1 (0 when empty).
    double value() const;

private:
    struct Entry
    {
        double progress = 0.0;
        qint64 weight = 1;
        qint64 updatedMs = 0;
    };
    QHash<QString, Entry> m_entries;
};
