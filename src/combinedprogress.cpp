#include "combinedprogress.h"

#include <cmath>

void CombinedProgress::set(const QString &id, double progress, qint64 totalBytes, qint64 nowMs)
{
    if (!std::isfinite(progress))
        progress = 0.0;
    m_entries.insert(id, Entry{qBound(0.0, progress, 1.0), qMax<qint64>(1, totalBytes), nowMs});
}

void CombinedProgress::remove(const QString &id)
{
    m_entries.remove(id);
}

bool CombinedProgress::expire(qint64 nowMs)
{
    return m_entries.removeIf([nowMs](const auto &it) { return nowMs - it.value().updatedMs > StaleMs; }) > 0;
}

double CombinedProgress::value() const
{
    // In doubles: the weights are byte counts, their sum may not fit a product with the progress in integers.
    double done = 0.0;
    double total = 0.0;
    for (const Entry &entry : m_entries) {
        done += entry.progress * double(entry.weight);
        total += double(entry.weight);
    }
    return total > 0.0 ? done / total : 0.0;
}
